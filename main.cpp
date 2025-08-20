///////////////////////////////////////////////////////////////////////////////
/// Point Cloud Distributed Processing System
///
/// 이 프로그램은 Master-Worker 패턴으로 Point Cloud 데이터를 분산처리
/// - Master: TCP 서버로서 청크 데이터를 워커에게 전달하고 결과를 수집
/// - Worker: Master에 접속하여 할당받은 청크를 처리하고 결과를 반환
/// - 원격 워커 자동 실행: (선택) SSH로 원격 머신에 워커 프로세스를 실행
///
/// 설계상 중요 포인트:
/// 1) 네트워크는 TCP 기반. 메시지 앞에 4바이트 정수형 변수로 전달될 메세지 길이 정보 제공
/// 2) Cross-Platform: Windows/Linux 모두 동작하도록 소켓 및 IP 탐지 코드를 분기 처리
/// 3) 원격 워커 자동 실행은 OpenSSH 클라이언트(ssh)가 PATH에 있다고 가정
/// 4) 설정 파일(workers.conf)로 원격 워커 목록을 관리
///
/// Point Cloud Distributed Processing System (Enhanced)
/// - Draining phase after all results: sends TERMINATE to late workers
/// - Worker self-termination on no-more-work/no-response
/// - SSH non-interactive options; timeouts/retries configurable via config
///////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <locale>
#include <codecvt>

/// Wide characters for Windows console (not used in Linux)
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/// Cross platform headers related with socket/network
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/select.h>
/// Linux: Definition for Windows compatibility
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket close
#endif

/// ---------- UTF-8/Wide helpers ----------
static std::string wstring_to_utf8(const std::wstring& wstr) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.to_bytes(wstr);
}
static std::wstring utf8_to_wstring(const std::string& str) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.from_bytes(str);
}

/// ---------- string utils ----------
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
///    아래 Linux 분기에서 인터페이스 이름(예: "eth0")을 조건에 추가
///  - IPv6 지원이 필요하면 AF_INET6 분기와 버퍼 크기(INET6_ADDRSTRLEN) 추가
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

/// ---------- Remote worker & settings ----------
struct RemoteWorkerConfig {
    std::string ip_address;
    std::string username;
    std::string worker_path;
    int ssh_port;
    RemoteWorkerConfig() : ssh_port(22) {}
    RemoteWorkerConfig(const std::string& ip, const std::string& user,
        const std::string& path, int port = 22)
        : ip_address(ip), username(user), worker_path(path), ssh_port(port) {
    }
};

/// 설정 파일 관리
/// - workers.conf를 읽고 RemoteWorkerConfig 목록을 생성
/// - 파일이 없을 경우 기본 템플릿 생성 함수 제공
struct RuntimeSettings {
    /// Master draining
    int drain_seconds = 5;

    /// Worker behavior
    int worker_idle_timeout_seconds = 20;   /// no work/no response duration → exit
    int worker_retry_max = 5;               /// max consecutive failures
    int worker_retry_backoff_ms = 200;      /// base backoff (ms)
    int worker_recv_timeout_ms = 5000;      /// SO_RCVTIMEO
    int worker_send_timeout_ms = 5000;      /// SO_SNDTIMEO

    /// SSH options for remote launching
    int  ssh_connect_timeout_sec = 5;
    int  ssh_server_alive_interval_sec = 5;
    int  ssh_server_alive_count_max = 2;
    bool ssh_batch_mode = true;             /// BatchMode=yes
    std::string ssh_strict_host_key = "accept-new"; /// accept-new / yes / no
};

/// ---------- Config manager ----------
class ConfigManager {
public:
    static void createDefaultConfig(const std::string& config_file) {
        std::ofstream file(config_file);
        if (!file.is_open()) return;

        file << "# Remote Worker Configuration File\n";
        file << "# Lines with commas are remote workers: ip,username,worker_path,ssh_port\n";
        file << "# Lines with key=value are settings.\n\n";

        file << "# ===== Settings (defaults) =====\n";
        file << "drain_seconds=5\n";
        file << "worker_idle_timeout_seconds=20\n";
        file << "worker_retry_max=5\n";
        file << "worker_retry_backoff_ms=200\n";
        file << "worker_recv_timeout_ms=5000\n";
        file << "worker_send_timeout_ms=5000\n";
        file << "ssh_connect_timeout_sec=5\n";
        file << "ssh_server_alive_interval_sec=5\n";
        file << "ssh_server_alive_count_max=2\n";
        file << "ssh_batch_mode=yes\n";
        file << "ssh_strict_host_key=accept-new\n\n";

        file << "# ===== Remote workers (examples) =====\n";
        file << "# 192.168.1.100,user1,/home/user1/point_cloud_worker,22\n";
        file << "# 192.168.1.101,user2,/opt/workers/point_cloud_worker,22\n";
        file << "# 10.0.0.50,admin,C:\\\\Workers\\\\point_cloud_worker.exe,22\n";
    }

    static void ensureConfig(const std::string& config_file) {
        std::ifstream f(config_file);
        if (!f.is_open()) {
            createDefaultConfig(config_file);
        }
    }

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
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            /// CSV line?
            if (line.find(',') != std::string::npos) {
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
        }
        return workers;
    }

    /// 기본 템플릿 생성
    /// - 초기에 파일이 없을 때 사용자에게 예시를 제공합니다.
    static RuntimeSettings loadRuntimeSettings(const std::string& config_file) {
        RuntimeSettings s;
        std::ifstream file(config_file);
        if (!file.is_open()) return s;

        std::string line;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.find('=') == std::string::npos || line.find(',') != std::string::npos) continue;

            auto pos = line.find('=');
            std::string k = trim(line.substr(0, pos));
            std::string v = trim(line.substr(pos + 1));

            auto lcase = [](std::string t) {
                std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {return std::tolower(c); });
                return t;
                };
            std::string lk = lcase(k), lv = lcase(v);

            try {
                if (lk == "drain_seconds") s.drain_seconds = std::stoi(v);
                else if (lk == "worker_idle_timeout_seconds") s.worker_idle_timeout_seconds = std::stoi(v);
                else if (lk == "worker_retry_max") s.worker_retry_max = std::stoi(v);
                else if (lk == "worker_retry_backoff_ms") s.worker_retry_backoff_ms = std::stoi(v);
                else if (lk == "worker_recv_timeout_ms") s.worker_recv_timeout_ms = std::stoi(v);
                else if (lk == "worker_send_timeout_ms") s.worker_send_timeout_ms = std::stoi(v);
                else if (lk == "ssh_connect_timeout_sec") s.ssh_connect_timeout_sec = std::stoi(v);
                else if (lk == "ssh_server_alive_interval_sec") s.ssh_server_alive_interval_sec = std::stoi(v);
                else if (lk == "ssh_server_alive_count_max") s.ssh_server_alive_count_max = std::stoi(v);
                else if (lk == "ssh_batch_mode") s.ssh_batch_mode = (lv == "1" || lv == "true" || lv == "yes" || lv == "on");
                else if (lk == "ssh_strict_host_key") s.ssh_strict_host_key = v;
            }
            catch (...) {
                std::wcout << L"[Config] Failed to parse key: " << k.c_str() << L"\n";
            }
        }
        return s;
    }
};

/// 원격 워커 실행 관리자
/// - ssh 명령으로 원격 머신에서 워커 프로세스를 실행
/// - 실패 시 재시도하며, stop() 호출 시 안전하게 스레드를 정리
/// 수정 가이드:
///  - plink/pscp 등 다른 SSH 클라이언트 사용 시 명령어 생성 부분 교체
///  - key 기반 인증을 전제로 하므로, ssh-copy-id 등으로 미리 키를 배포
class RemoteWorkerManager {
private:
    std::vector<RemoteWorkerConfig> remote_configs;
    std::vector<std::thread> remote_threads;
    std::string master_ip;
    int master_port;
    RuntimeSettings settings;
    std::atomic<bool> should_stop{ false };

public:
    RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
        const std::string& master_ip, int master_port,
        const RuntimeSettings& settings)
        : remote_configs(configs), master_ip(master_ip),
        master_port(master_port), settings(settings) {
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
            << " -o ConnectTimeout=" << settings.ssh_connect_timeout_sec
            << " -o ServerAliveInterval=" << settings.ssh_server_alive_interval_sec
            << " -o ServerAliveCountMax=" << settings.ssh_server_alive_count_max
            << " -o StrictHostKeyChecking=" << settings.ssh_strict_host_key;
        if (settings.ssh_batch_mode) ssh_command << " -o BatchMode=yes";
        ssh_command
            << " " << config.username << "@" << config.ip_address
            << " \"" << config.worker_path
            << " worker " << master_ip << " " << master_port << " " << "workers.conf" << "\""
            << " < NUL"; /// detach stdin
#else
        ssh_command << "ssh -p " << config.ssh_port
            << " -o ConnectTimeout=" << settings.ssh_connect_timeout_sec
            << " -o ServerAliveInterval=" << settings.ssh_server_alive_interval_sec
            << " -o ServerAliveCountMax=" << settings.ssh_server_alive_count_max
            << " -o ExitOnForwardFailure=yes"
            << " -o StrictHostKeyChecking=" << settings.ssh_strict_host_key
            << " -n"; /// detach stdin
        if (settings.ssh_batch_mode) ssh_command << " -o BatchMode=yes";
        ssh_command
            << " " << config.username << "@" << config.ip_address
            << " '" << config.worker_path
            << " worker " << master_ip << " " << master_port << " " << "workers.conf" << "'";
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
            if (!should_stop.load()) std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        std::wcout << L"Remote worker thread for " << config.ip_address.c_str() << L" terminated\n";
    }
};

/// ---------- Point / Chunk / Result ----------
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

/// ---------- Master ----------
class MasterServer {
private:
    std::vector<PointCloudChunk> chunks; /// 전송할 chunk
    std::vector<ProcessResult>   results; /// 수집된 결과
    std::atomic<int>             chunk_index{ 0 };
    std::mutex                   results_mutex;
    int                          port;
    std::atomic<bool>            all_chunks_processed{ false };
    std::unique_ptr<RemoteWorkerManager> remote_manager;
    std::string                  config_file;
    RuntimeSettings              settings;
    std::vector<RemoteWorkerConfig> remote_configs;

public:
    MasterServer(int p, const std::string& config = "workers.conf")
        : port(p), config_file(config) {
    }

    ~MasterServer() {
        if (remote_manager) remote_manager->stop();
    }

    /// 테스트를 위한 샘플 생성
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
        /// Load config (create default if missing)
        ConfigManager::ensureConfig(config_file);
        settings = ConfigManager::loadRuntimeSettings(config_file);
        remote_configs = ConfigManager::loadRemoteWorkers(config_file);

        /// Create listening socket
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock == INVALID_SOCKET) { std::wcerr << L"Socket creation failed\n"; return; }

        int opt = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons(port);
        if (bind(server_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::wcerr << L"Bind failed\n"; closesocket(server_sock); return;
        }
        if (listen(server_sock, 10) == SOCKET_ERROR) {
            std::wcerr << L"Listen failed\n"; closesocket(server_sock); return;
        }

        std::wcout << L"Master server listening on port " << port << L"\n";

        /// Start remote workers AFTER listening is ready
        if (!remote_configs.empty()) {
            std::string master_ip = getLocalIPAddress();
            remote_manager = std::make_unique<RemoteWorkerManager>(remote_configs, master_ip, port, settings);
            std::wcout << L"Starting remote workers (Master IP: "
                << master_ip.c_str() << L":" << port << L")...\n";
            remote_manager->startRemoteWorkers();
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        else {
            std::wcout << L"No remote workers configured. Running with local workers only.\n";
        }

        /// Draining-aware accept loop
        bool draining = false;
        auto drain_deadline = (std::chrono::steady_clock::time_point::max)();

        while (true) {
            /// Select with 1s timeout to check draining deadline
            fd_set rfds; FD_ZERO(&rfds); FD_SET(server_sock, &rfds);
            timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
#ifdef _WIN32
            int ready = select(0, &rfds, nullptr, nullptr, &tv); /// nfds ignored on Windows
#else
            int ready = select(server_sock + 1, &rfds, nullptr, nullptr, &tv);
#endif
            if (ready <= 0) {
                if (draining && std::chrono::steady_clock::now() >= drain_deadline) break;
                continue;
            }

            sockaddr_in client_addr{}; socklen_t client_len = sizeof(client_addr);
            SOCKET client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_sock == INVALID_SOCKET) continue;

            char ipStr[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, INET_ADDRSTRLEN);
            std::wcout << L"Worker connected: " << ipStr << L"\n";

            int idx = (!draining) ? chunk_index.fetch_add(1) : INT32_MAX;

            if (!draining && idx < static_cast<int>(chunks.size())) {
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
                                draining = true;
                                drain_deadline = std::chrono::steady_clock::now()
                                    + std::chrono::seconds(settings.drain_seconds);
                                std::wcout << L"All chunks processed! Entering draining phase for "
                                    << settings.drain_seconds << L"s...\n";
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
};

/// ---------- Worker ----------
class WorkerClient {
private:
    std::string master_ip;
    int master_port;
    RuntimeSettings settings;
    bool last_was_terminate_ = false;

public:
    WorkerClient(const std::string& ip, int port, const RuntimeSettings& s)
        : master_ip(ip), master_port(port), settings(s) {
    }

    void start() {
        using clock = std::chrono::steady_clock;
        auto last_success = clock::now();
        int fail_count = 0;

        std::wcout << L"Worker started. Connecting to master...\n";
        while (true) {
            if (!connectAndProcess()) {
                if (last_was_terminate_) {
                    std::wcout << L"Worker terminated by master.\n"; break;
                }
                auto idle_elapsed = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - last_success).count();
                if (idle_elapsed >= settings.worker_idle_timeout_seconds || ++fail_count >= settings.worker_retry_max) {
                    std::wcout << L"No more work or no response from master. Exiting.\n"; break;
                }
                int backoff = settings.worker_retry_backoff_ms * fail_count;
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                continue;
            }
            /// success
            fail_count = 0;
            last_success = clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

private:
    bool connectAndProcess() {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) { std::wcerr << L"Socket creation failed\n"; return false; }

        /// Apply send/recv timeouts
#ifdef _WIN32
        DWORD rcv = settings.worker_recv_timeout_ms;
        DWORD snd = settings.worker_send_timeout_ms;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof(snd));
#else
        auto to_tv = [](int ms) {
            timeval tv{};
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            return tv;
            };
        timeval rtv = to_tv(settings.worker_recv_timeout_ms);
        timeval stv = to_tv(settings.worker_send_timeout_ms);
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
#endif

        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(master_port);
        if (inet_pton(AF_INET, master_ip.c_str(), &addr.sin_addr) != 1) {
            std::wcerr << L"Invalid IP address\n"; closesocket(sock); return false;
        }

        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::wcerr << L"Connection to master failed\n"; closesocket(sock); return false;
        }

        std::string received_data = NetworkUtils::receiveData(sock);
        if (received_data.empty()) {
            std::wcerr << L"Failed to receive data from master\n"; closesocket(sock);
            last_was_terminate_ = false; return false;
        }

        if (NetworkUtils::isTerminationSignal(received_data)) {
            std::wcout << L"Received termination signal from master\n";
            closesocket(sock); last_was_terminate_ = true; return false;
        }

        PointCloudChunk chunk = PointCloudChunk::deserialize(received_data);
        std::wcout << L"Received chunk " << chunk.chunk_id
            << L" with " << chunk.points.size() << L" points\n";

        ProcessResult result = processPointCloud(chunk);
        NetworkUtils::sendData(sock, result.serialize());
        std::wcout << L"Sent processing result for chunk " << chunk.chunk_id << L"\n";

        closesocket(sock);
        last_was_terminate_ = false;
        return true;
    }

    ProcessResult processPointCloud(const PointCloudChunk& chunk) {
        std::wcout << L"Processing chunk " << chunk.chunk_id << L"...\n";
        float total_distance = 0.0f;
        for (const auto& p : chunk.points) {
            float d = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            total_distance += d;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); /// demo delay

        ProcessResult r; r.chunk_id = chunk.chunk_id;
        r.avg_distance = total_distance / static_cast<float>(chunk.points.size());
        r.point_count = static_cast<int>(chunk.points.size());
        std::wcout << L"Finished processing chunk " << chunk.chunk_id
            << L" (avg_distance: " << r.avg_distance << L")\n";
        return r;
    }
};

/// ---------- main / wmain ----------
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::wcerr << L"WSAStartup failed\n"; return 1;
    }

    if (argc < 2) {
        std::wcout << L"Usage:\n";
        std::wcout << L"  Master mode: " << argv[0] << L" master [port] [config_file]\n";
        std::wcout << L"  Worker mode: " << argv[0] << L" worker [master_ip] [master_port] [config_file]\n";
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
        std::string cfg = (argc >= 5) ? wstring_to_utf8(argv[4]) : "workers.conf";
        ConfigManager::ensureConfig(cfg);
        RuntimeSettings s = ConfigManager::loadRuntimeSettings(cfg);
        WorkerClient worker(wstring_to_utf8(master_ip_w), master_port, s);
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
        std::cout << "  Worker mode: " << argv[0] << " worker [master_ip] [master_port] [config_file]\n";
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
        std::string cfg = (argc >= 5) ? argv[4] : "workers.conf";
        ConfigManager::ensureConfig(cfg);
        RuntimeSettings s = ConfigManager::loadRuntimeSettings(cfg);
        WorkerClient worker(ip, port, s);
        worker.start();
    }
    else {
        std::cerr << "Invalid mode. Use 'master' or 'worker'\n";
        return 1;
    }
    return 0;
}
#endif
