///////////////////////////////////////////////////////////////////////////////
/// Point Cloud Distributed Processing System
///
/// 이 프로그램은 Master-Worker 패턴으로 Point Cloud 데이터를 분산 처리합니다.
/// - Master: TCP 서버로서 청크 데이터를 워커에게 전달하고 결과를 수집합니다.
/// - Worker: Master에 접속하여 할당받은 청크를 처리하고 결과를 반환합니다.
/// - 원격 워커 자동 실행: (선택) SSH로 원격 머신에 워커 프로세스를 실행합니다.
///
/// 설계상 중요 포인트:
/// 1) 네트워크는 TCP 기반이며, 메시지 앞에 4바이트 길이(정수)를 보내는 단순 프로토콜입니다.
/// 2) Cross-Platform: Windows/Linux 모두 동작하도록 소켓 및 IP 탐지 코드를 분기 처리했습니다.
/// 3) 원격 워커 자동 실행은 OpenSSH 클라이언트(ssh)가 PATH에 있다고 가정합니다.
/// 4) 설정 파일(workers.conf)로 원격 워커 목록을 관리합니다.
/// 5) 유지보수 포인트에 '/// 수정 가이드' 주석을 넣었습니다.
///////////////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <thread>
#include <string>
#include <sstream>
#include <chrono>
#include <random>
#include <atomic>
#include <mutex>
#include <fstream>
#include <algorithm>
#include <memory>
#include <cmath>

#include <locale>
#include <codecvt>

/// 유니코드 변환 유틸리티: Windows에서 wmain과 UTF-8 문자열 상호 변환에 사용됩니다.
/// - 외부 파일 경로, config 파일에서 읽은 문자열을 넓은문자/UTF-8로 바꿔 출력할 때 유용합니다.
static std::string wstring_to_utf8(const std::wstring& wstr) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.to_bytes(wstr);
}
static std::wstring utf8_to_wstring(const std::string& str) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.from_bytes(str);
}

/// Windows 콘솔 wide 문자 모드 설정용 헤더 (Linux에선 미사용)
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/// 소켓/네트워크 관련 크로스플랫폼 헤더
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
/// Windows: SOCKET 타입/매크로 이미 정의됨
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
/// Linux: Windows 호환을 위한 정의
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket close
#endif

/// 문자열 유틸: CSV 파싱과 자잘한 전처리에 사용됩니다.
static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out; std::stringstream ss(s); std::string tok;
    while (std::getline(ss, tok, delim)) out.push_back(tok);
    return out;
}
static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// 로컬 IP 자동 탐지(IPv4)
/// - Windows: getaddrinfo + inet_ntop
/// - Linux:   getifaddrs + inet_ntop (루프백 제외)
/// 반환 실패 시 127.0.0.1
/// 수정 가이드:
///  - 멀티 NIC 환경에서 특정 인터페이스 우선순위를 두고 싶다면
///    아래 Linux 분기에서 인터페이스 이름(예: "eth0")을 조건에 추가하세요.
///  - IPv6 지원이 필요하면 AF_INET6 분기와 버퍼 크기(INET6_ADDRSTRLEN) 추가.
static std::string getLocalIPAddress() {
    std::string local_ip = "127.0.0.1";
#ifdef _WIN32
    char hostname[256] = { 0 };
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return local_ip;
    }
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || !result) {
        return local_ip;
    }
    char ipStr[INET_ADDRSTRLEN] = { 0 };
    auto* a = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    if (inet_ntop(AF_INET, &(a->sin_addr), ipStr, sizeof(ipStr))) {
        local_ip = ipStr;
    }
    freeaddrinfo(result);
#else
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) return local_ip;
    for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa || !ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET && !(ifa->ifa_flags & IFF_LOOPBACK)) {
            char ip[INET_ADDRSTRLEN] = { 0 };
            void* addr_ptr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
            if (inet_ntop(AF_INET, addr_ptr, ip, sizeof(ip))) {
                local_ip = ip; break;
            }
        }
    }
    freeifaddrs(ifaddr);
#endif
    return local_ip;
}

/// 원격 워커 정보를 담는 구조체
/// - workers.conf에서 파싱해 채워집니다.
struct RemoteWorkerConfig {
    std::string ip_address;   /// SSH 접속할 워커 IP
    std::string username;     /// SSH 사용자명
    std::string worker_path;  /// 원격 워커 실행 파일/스크립트 경로
    int ssh_port;             /// SSH 포트 (기본 22)
    RemoteWorkerConfig() : ssh_port(22) {}
    RemoteWorkerConfig(const std::string& ip, const std::string& user,
        const std::string& path, int port = 22)
        : ip_address(ip), username(user), worker_path(path), ssh_port(port) {
    }
};

/// 설정 파일 관리
/// - workers.conf를 읽고 RemoteWorkerConfig 목록을 생성합니다.
/// - 파일이 없을 경우 기본 템플릿 생성 함수 제공.
class ConfigManager {
public:
    static std::vector<RemoteWorkerConfig> loadRemoteWorkers(const std::string& config_file) {
        std::vector<RemoteWorkerConfig> workers;
        std::ifstream file(config_file);
        if (!file.is_open()) {
            std::wcout << L"Config file not found: " << config_file.c_str()
                << L". Running with local workers only.\n";
            return workers;
        }
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto tokens = split(line, ',');
            if (tokens.size() >= 3) {
                RemoteWorkerConfig cfg;
                cfg.ip_address = trim(tokens[0]);
                cfg.username = trim(tokens[1]);
                cfg.worker_path = trim(tokens[2]);
                if (tokens.size() >= 4) cfg.ssh_port = std::stoi(trim(tokens[3]));
                workers.push_back(cfg);
                std::wcout << L"Loaded remote worker: " << cfg.ip_address.c_str()
                    << L" (" << cfg.username.c_str() << L")\n";
            }
        }
        return workers;
    }

    /// 기본 템플릿 생성
    /// - 초기에 파일이 없을 때 사용자에게 예시를 제공합니다.
    static void createDefaultConfig(const std::string& config_file) {
        std::ofstream file(config_file);
        if (file.is_open()) {
            file << "# Remote Worker Configuration File\n";
            file << "# Format: ip_address,username,worker_path,ssh_port\n";
            file << "# Example configurations (uncomment and modify as needed):\n";
            file << "# 192.168.1.100,user1,/home/user1/point_cloud_worker,22\n";
            file << "# 192.168.1.101,user2,/opt/workers/point_cloud_worker,22\n";
            file << "# 10.0.0.50,admin,C:\\\\Workers\\\\point_cloud_worker.exe,22\n";
            std::wcout << L"Created default config file: " << config_file.c_str() << L"\n";
        }
    }
};

/// 원격 워커 실행 관리자
/// - ssh 명령으로 원격 머신에서 워커 프로세스를 실행합니다.
/// - 실패 시 재시도하며, stop() 호출 시 안전하게 스레드를 정리합니다.
/// 수정 가이드:
///  - plink/pscp 등 다른 SSH 클라이언트 사용 시 명령어 생성 부분을 교체하세요.
///  - key 기반 인증을 전제로 하므로, ssh-copy-id 등으로 미리 키를 배포해야 합니다.
class RemoteWorkerManager {
private:
    std::vector<RemoteWorkerConfig> remote_configs;
    std::vector<std::thread> remote_threads;
    std::string master_ip;
    int master_port;
    std::atomic<bool> should_stop{ false };

public:
    RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
        const std::string& master_ip, int master_port)
        : remote_configs(configs), master_ip(master_ip), master_port(master_port) {
    }

    ~RemoteWorkerManager() { stop(); }

    void startRemoteWorkers() {
        for (const auto& cfg : remote_configs) {
            remote_threads.emplace_back([this, cfg]() { this->runRemoteWorker(cfg); });
        }
        std::wcout << L"Started " << remote_configs.size() << L" remote workers\n";
    }

    void stop() {
        should_stop.store(true);
        for (auto& t : remote_threads) if (t.joinable()) t.join();
        remote_threads.clear();
    }

private:
    void runRemoteWorker(const RemoteWorkerConfig& config) {
        std::wcout << L"Starting remote worker on " << config.ip_address.c_str() << L"\n";
        std::stringstream ssh_command;
#ifdef _WIN32
        /// Windows: OpenSSH 클라이언트(ssh)가 PATH에 있어야 합니다.
        ssh_command << "ssh -p " << config.ssh_port
            << " " << config.username << "@" << config.ip_address
            << " \"" << config.worker_path
            << " worker " << master_ip << " " << master_port << "\"";
#else
        /// Linux/macOS: 기본 ssh 사용
        ssh_command << "ssh -p " << config.ssh_port
            << " " << config.username << "@" << config.ip_address
            << " '" << config.worker_path
            << " worker " << master_ip << " " << master_port << "'";
#endif
        const std::string cmd = ssh_command.str();
        std::wcout << L"Executing: " << cmd.c_str() << L"\n";

        while (!should_stop.load()) {
            int rc = std::system(cmd.c_str());
            if (rc == 0) {
                std::wcout << L"Remote worker on " << config.ip_address.c_str()
                    << L" completed successfully\n";
            }
            else {
                std::wcout << L"Remote worker on " << config.ip_address.c_str()
                    << L" failed or disconnected (code: " << rc << L")\n";
            }
            if (!should_stop.load()) std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        std::wcout << L"Remote worker thread for " << config.ip_address.c_str() << L" terminated\n";
    }
};

/// 3D 포인트 자료형 및 청크 직렬화/역직렬화
/// - 네트워크 전송은 텍스트 기반이지만, 길이 프레이밍으로 안전하게 보냅니다.
struct Point3D {
    float x, y, z;
    Point3D(float x = 0, float y = 0, float z = 0) : x(x), y(y), z(z) {}
};

struct PointCloudChunk {
    int chunk_id{};
    std::vector<Point3D> points;

    std::string serialize() const {
        std::stringstream ss;
        ss << chunk_id << " " << points.size() << " ";
        for (const auto& p : points) ss << p.x << " " << p.y << " " << p.z << " ";
        return ss.str();
    }
    static PointCloudChunk deserialize(const std::string& data) {
        PointCloudChunk c; std::stringstream ss(data); size_t n = 0; ss >> c.chunk_id >> n;
        c.points.reserve(n);
        for (size_t i = 0; i < n; ++i) { Point3D p; ss >> p.x >> p.y >> p.z; c.points.push_back(p); }
        return c;
    }
};

/// 처리 결과 구조체
struct ProcessResult {
    int   chunk_id{};
    float avg_distance{};
    int   point_count{};
    std::string serialize() const {
        std::stringstream ss; ss << chunk_id << " " << avg_distance << " " << point_count; return ss.str();
    }
    static ProcessResult deserialize(const std::string& s) {
        ProcessResult r; std::stringstream ss(s); ss >> r.chunk_id >> r.avg_distance >> r.point_count; return r;
    }
};

/// 네트워크 유틸리티
/// - send/recv는 길이 프레이밍을 이용합니다(먼저 4바이트 길이, 그 다음 페이로드).
/// 수정 가이드:
///  - 타임아웃이나 논블로킹 소켓을 사용하려면 select/poll/epoll을 추가로 감싸세요.
///  - 큰 데이터 전송 시 압축(zstd) 등을 추가 가능.
class NetworkUtils {
public:
    static bool sendData(SOCKET s, const std::string& data) {
        uint32_t size = static_cast<uint32_t>(data.size());
        if (send(s, reinterpret_cast<const char*>(&size), sizeof(size), 0) != sizeof(size)) return false;
        const char* buf = data.data();
        int total = 0, need = static_cast<int>(size);
        while (total < need) {
            int sent = send(s, buf + total, need - total, 0);
            if (sent == SOCKET_ERROR) return false;
            total += sent;
        }
        return true;
    }

    static std::string receiveData(SOCKET s) {
        uint32_t size = 0;
        int recvd = recv(s, reinterpret_cast<char*>(&size), sizeof(size), 0);
        if (recvd != sizeof(size)) return "";
        std::string data(size, '\0');
        int total = 0, need = static_cast<int>(size);
        while (total < need) {
            int got = recv(s, &data[total], need - total, 0);
            if (got == SOCKET_ERROR || got == 0) return "";
            total += got;
        }
        return data;
    }

    static bool sendTerminationSignal(SOCKET s) {
        static const std::string k = "TERMINATE";
        return sendData(s, k);
    }
    static bool isTerminationSignal(const std::string& d) { return d == "TERMINATE"; }
};

/// Master 서버
/// - 샘플 데이터를 생성하고, TCP 서버를 열어 워커 연결을 처리합니다.
/// - 모든 청크가 처리되면 결과를 출력하고 종료합니다.
class MasterServer {
private:
    std::vector<PointCloudChunk> chunks;     /// 전송할 청크들
    std::vector<ProcessResult>   results;    /// 수집된 결과
    std::atomic<int>             chunk_index{ 0 };
    std::mutex                   results_mutex;
    int                          port;
    std::atomic<bool>            all_chunks_processed{ false };
    std::unique_ptr<RemoteWorkerManager> remote_manager;
    std::string                  config_file;

public:
    MasterServer(int p, const std::string& config = "workers.conf")
        : port(p), config_file(config) {
    }

    ~MasterServer() {
        if (remote_manager) remote_manager->stop();
    }

    /// 테스트용 샘플 데이터 생성 (10 청크 * 1000 포인트)
    /// 수정 가이드:
    ///  - 실제 데이터 파이프라인(파일 로드 등)과 연동하려면 이 부분을 교체하세요.
    void generateSampleData() {
        std::random_device rd; std::mt19937 gen(rd());
        std::uniform_real_distribution<float> dis(-100.0f, 100.0f);
        for (int cid = 0; cid < 10; ++cid) {
            PointCloudChunk c; c.chunk_id = cid;
            for (int i = 0; i < 1000; ++i) c.points.emplace_back(dis(gen), dis(gen), dis(gen));
            chunks.push_back(std::move(c));
        }
        std::wcout << L"Generated " << chunks.size() << L" chunks with total "
            << chunks.size() * 1000 << L" points\n";
    }

    /// 서버 시작:
    /// 1) (옵션) 원격 워커 자동 실행
    /// 2) TCP listen → accept 루프에서 워커에 청크 전송/결과 수신
    void start() {
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock == INVALID_SOCKET) {
            std::wcerr << L"Socket creation failed\n";
            return;
        }

        int opt = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (bind(server_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::wcerr << L"Bind failed\n";
            closesocket(server_sock);
            return;
        }
        if (listen(server_sock, 10) == SOCKET_ERROR) {
            std::wcerr << L"Listen failed\n";
            closesocket(server_sock);
            return;
        }

        std::wcout << L"Master server listening on port " << port << L"\n";

        // 🎯 여기서 원격 워커 실행 (리스닝 준비 완료 후)
        setupRemoteWorkers();

        while (!all_chunks_processed.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            SOCKET client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_sock == INVALID_SOCKET) continue;

            char ipStr[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, INET_ADDRSTRLEN);
            std::wcout << L"Worker connected: " << ipStr << L"\n";

            int idx = chunk_index.fetch_add(1);
            if (idx < static_cast<int>(chunks.size())) {
                std::string chunk_data = chunks[idx].serialize();
                if (NetworkUtils::sendData(client_sock, chunk_data)) {
                    std::wcout << L"Sent chunk " << idx << L" to worker\n";
                    std::string result_data = NetworkUtils::receiveData(client_sock);
                    if (!result_data.empty()) {
                        ProcessResult r = ProcessResult::deserialize(result_data);
                        {
                            std::lock_guard<std::mutex> lk(results_mutex);
                            results.push_back(r);
                            std::wcout << L"Received result for chunk " << r.chunk_id
                                << L": avg_distance=" << r.avg_distance
                                << L", points=" << r.point_count << L"\n";
                            if (results.size() == chunks.size()) {
                                all_chunks_processed.store(true);
                                std::wcout << L"All chunks processed!\n";
                            }
                        }
                    }
                }
            }
            else {
                NetworkUtils::sendTerminationSignal(client_sock);
                std::wcout << L"Sent termination signal to worker\n";
            }
            closesocket(client_sock);
        }

        closesocket(server_sock);
        printFinalResults();

        if (remote_manager) {
            std::wcout << L"Shutting down remote workers...\n";
            remote_manager->stop();
        }
    }


private:
    /// 최종 결과 출력
    /// - 평균거리 평균과 총 포인트 수를 출력합니다.
    void printFinalResults() {
        std::wcout << L"\n=== Final Results ===\n";
        float total_avg = 0.0f; int total_points = 0;
        for (const auto& r : results) {
            total_avg += r.avg_distance; total_points += r.point_count;
            std::wcout << L"Chunk " << r.chunk_id << L": " << r.point_count
                << L" points, avg_distance=" << r.avg_distance << L"\n";
        }
        if (!results.empty()) {
            std::wcout << L"Overall average distance: " << (total_avg / results.size()) << L"\n";
            std::wcout << L"Total points processed: " << total_points << L"\n";
        }
    }

    /// 원격 워커 자동 실행
    /// - config 파일이 없으면 템플릿 생성 → 로드 → SSH로 각 워커 실행
    /// - 워커는 "worker <master_ip> <master_port>" 인수로 실행되어야 합니다.
    void setupRemoteWorkers() {
        std::ifstream test_file(config_file);
        if (!test_file.is_open()) {
            ConfigManager::createDefaultConfig(config_file);
        }

        auto remote_configs = ConfigManager::loadRemoteWorkers(config_file);
        if (!remote_configs.empty()) {
            std::string master_ip = getLocalIPAddress();
            remote_manager = std::make_unique<RemoteWorkerManager>(remote_configs, master_ip, port);

            std::wcout << L"Starting remote workers (Master IP: "
                << master_ip.c_str() << L":" << port << L")...\n";
            remote_manager->startRemoteWorkers();
            /// 워커가 뜨는 시간 확보 (필요시 조정)
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        else {
            std::wcout << L"No remote workers configured. Running with local workers only.\n";
        }
    }
};

/// Worker 클라이언트
/// - Master와 TCP로 통신하여 청크를 받아 처리하고 결과를 반환합니다.
/// - 종료 신호("TERMINATE")를 받으면 루프를 종료합니다.
class WorkerClient {
private:
    std::string master_ip;
    int master_port;

public:
    WorkerClient(const std::string& ip, int port) : master_ip(ip), master_port(port) {}

    void start() {
        std::wcout << L"Worker started. Connecting to master...\n";
        while (true) {
            if (!connectAndProcess()) {
                std::wcout << L"Worker terminated.\n";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

private:
    bool connectAndProcess() {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) { std::wcerr << L"Socket creation failed\n"; return false; }

        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(master_port);
        if (inet_pton(AF_INET, master_ip.c_str(), &addr.sin_addr) != 1) {
            std::wcerr << L"Invalid IP address\n"; closesocket(sock); return false;
        }

        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::wcerr << L"Connection to master failed\n"; closesocket(sock); return false;
        }

        std::string received_data = NetworkUtils::receiveData(sock);
        if (received_data.empty()) { std::wcerr << L"Failed to receive data from master\n"; closesocket(sock); return false; }

        if (NetworkUtils::isTerminationSignal(received_data)) {
            std::wcout << L"Received termination signal from master\n"; closesocket(sock); return false;
        }

        PointCloudChunk chunk = PointCloudChunk::deserialize(received_data);
        std::wcout << L"Received chunk " << chunk.chunk_id
            << L" with " << chunk.points.size() << L" points\n";

        ProcessResult result = processPointCloud(chunk);
        NetworkUtils::sendData(sock, result.serialize());
        std::wcout << L"Sent processing result for chunk " << chunk.chunk_id << L"\n";

        closesocket(sock);
        return true;
    }

    /// 실제 처리 로직(데모): 각 포인트의 원점 거리의 평균 계산
    /// 수정 가이드:
    ///  - 실제 알고리즘으로 교체하고, 연산 시간을 고려해 스레드/벡터화 등을 적용하세요.
    ProcessResult processPointCloud(const PointCloudChunk& chunk) {
        std::wcout << L"Processing chunk " << chunk.chunk_id << L"...\n";
        float total_distance = 0.0f;
        for (const auto& p : chunk.points) {
            float d = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            total_distance += d;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); /// 데모용 지연

        ProcessResult r; r.chunk_id = chunk.chunk_id;
        r.avg_distance = total_distance / static_cast<float>(chunk.points.size());
        r.point_count = static_cast<int>(chunk.points.size());
        std::wcout << L"Finished processing chunk " << chunk.chunk_id
            << L" (avg_distance: " << r.avg_distance << L")\n";
        return r;
    }
};

/// 프로그램 진입점
/// - Windows: wmain(넓은문자) + WSAStartup/WSACleanup
/// - Linux:   main(일반문자)
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_U16TEXT);  /// 콘솔을 wide 모드로
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::wcerr << L"WSAStartup failed\n"; return 1;
    }

    if (argc < 2) {
        std::wcout << L"Usage:\n";
        std::wcout << L"  Master mode: " << argv[0] << L" master [port] [config_file]\n";
        std::wcout << L"  Worker mode: " << argv[0] << L" worker [master_ip] [master_port]\n";
        WSACleanup(); return 1;
    }

    std::wstring mode = argv[1];
    if (mode == L"master") {
        int port = (argc >= 3) ? _wtoi(argv[2]) : 8080;
        std::string cfg = (argc >= 4) ? wstring_to_utf8(argv[3]) : "workers.conf";
        MasterServer server(port, cfg);
        server.generateSampleData();
        server.start();
    }
    else if (mode == L"worker") {
        std::wstring master_ip_w = (argc >= 3) ? argv[2] : L"127.0.0.1";
        int master_port = (argc >= 4) ? _wtoi(argv[3]) : 8080;
        WorkerClient worker(wstring_to_utf8(master_ip_w), master_port);
        worker.start();
    }
    else {
        std::wcerr << L"Invalid mode. Use 'master' or 'worker'\n";
        WSACleanup(); return 1;
    }

    WSACleanup();
    return 0;
}
#else
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  Master mode: " << argv[0] << " master [port] [config_file]\n";
        std::cout << "  Worker mode: " << argv[0] << " worker [master_ip] [master_port]\n";
        return 1;
    }
    std::string mode = argv[1];
    if (mode == "master") {
        int port = (argc >= 3) ? std::stoi(argv[2]) : 8080;
        std::string cfg = (argc >= 4) ? argv[3] : "workers.conf";
        MasterServer server(port, cfg);
        server.generateSampleData();
        server.start();
    }
    else if (mode == "worker") {
        std::string ip = (argc >= 3) ? argv[2] : "127.0.0.1";
        int port = (argc >= 4) ? std::stoi(argv[3]) : 8080;
        WorkerClient worker(ip, port);
        worker.start();
    }
    else {
        std::cerr << "Invalid mode. Use 'master' or 'worker'\n";
        return 1;
    }
    return 0;
}
#endif
