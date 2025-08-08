///////////////////////////////////////////////////////////////////////////////
/// Point Cloud Distributed Processing System
/// 
/// 프로그램 목적:
/// - 대용량 Point Cloud 데이터를 여러 워커 노드에 분산 처리하는 시스템
/// - Master-Worker 패턴을 사용한 분산 컴퓨팅 구현
/// - TCP 소켓 통신을 통한 네트워크 기반 작업 분배
/// - 로컬/원격 워커 자동 실행 기능
/// 
/// 전체 흐름:
/// 1. Master: Point Cloud 데이터를 청크 단위로 분할하여 생성
/// 2. Master: 설정 파일에서 원격 워커 IP 목록 로드 (선택적)
/// 3. Master: 원격 워커들을 SSH를 통해 자동 실행 (설정된 경우)
/// 4. Master: TCP 서버를 시작하고 워커의 연결을 대기
/// 5. Worker: Master에 연결하여 처리할 청크 요청
/// 6. Master: 청크를 워커에게 전송
/// 7. Worker: 청크 데이터 처리 (원점으로부터의 평균 거리 계산)
/// 8. Worker: 처리 결과를 Master에게 반환
/// 9. Master: 모든 청크 처리 완료 시 최종 결과 출력
/// 
/// 주요 특징:
/// - 멀티스레딩 지원으로 동시 다중 워커 처리 가능
/// - Windows/Linux 크로스 플랫폼 호환성
/// - 데이터 직렬화/역직렬화를 통한 네트워크 전송
/// - 원자적 연산을 통한 스레드 안전성 보장
/// - 설정 파일 기반 원격 워커 자동 실행
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
#include <filesystem> /// std::filesystem은 C++17부터 지원됨
namespace fs = std::filesystem;

#ifdef _WIN32

/// Windows 플랫폼용 헤더 파일들
#include <fcntl.h>   /// _O_U16TEXT 정의를 위해 필요
#include <io.h>      /// _setmode 정의를 위해 필요

/// Windows 크로스 플랫폼 소켓 라이브러리
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

/// [Begin] C++17 이후로 사용X
// #include <locale>
// #include <codecvt>

/// 유니코드 문자열 변환 유틸리티 함수들
/// wstring과 UTF-8 string 간의 변환을 처리
//std::string wstring_to_utf8(const std::wstring& wstr) {
//    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
//    return conv.to_bytes(wstr);
//}
/// [End] C++17 이후로 사용X

//std::wstring utf8_to_wstring(const std::string& str) {
//    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
//    return conv.from_bytes(str);
//}

/// [Begin] 대안방법
#include <Windows.h>
std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}
/// [End] 대안방법
#else
/// Linux/Unix 환경에서는 iconv 또는 간단한 ASCII 변환 사용
#include <locale>
#include <cwchar>

std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    // UTF-8 locale 설정
    std::locale utf8_locale("en_US.UTF-8");
    std::locale::global(utf8_locale);

    // wcstombs를 사용한 변환
    size_t len = std::wcstombs(nullptr, wstr.c_str(), 0);
    if (len == static_cast<size_t>(-1)) {
        // 변환 실패 시 간단한 ASCII 변환 사용
        std::string result;
        result.reserve(wstr.length());
        for (wchar_t wc : wstr) {
            if (wc < 128) {
                result.push_back(static_cast<char>(wc));
            }
            else {
                result.push_back('?'); // non-ASCII 문자는 '?'로 대체
            }
        }
        return result;
    }

    std::string result(len, '\0');
    std::wcstombs(&result[0], wstr.c_str(), len);
    return result;
}

std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();

    // UTF-8 locale 설정
    std::locale utf8_locale("en_US.UTF-8");
    std::locale::global(utf8_locale);

    // mbstowcs를 사용한 변환
    size_t len = std::mbstowcs(nullptr, str.c_str(), 0);
    if (len == static_cast<size_t>(-1)) {
        // 변환 실패 시 간단한 ASCII 변환 사용
        std::wstring result;
        result.reserve(str.length());
        for (char c : str) {
            result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        return result;
    }

    std::wstring result(len, L'\0');
    std::mbstowcs(&result[0], str.c_str(), len);
    return result;
}

/// Linux 크로스 플랫폼 소켓 라이브러리
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

/// Windows/Linux 호환성을 위한 매크로 정의
#ifdef _WIN32
typedef int socklen_t;
#define close closesocket
/// MSG_WAITALL은 이미 winsock2.h에 정의되어 있음
#endif

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
// #define TINYGLTF_NOEXCEPTION // optional. disable exception handling.
#include "./gltf/tiny_gltf.h"

#include "face.h"

using namespace tinygltf;

const float FEET_TO_METER = 0.3048f;

/// 원격 워커 설정 정보를 담는 구조체
struct RemoteWorkerConfig {
    std::string ip_address;        /// 원격 컴퓨터 IP 주소
    std::string username;          /// SSH 로그인 사용자명
    std::string worker_path;       /// 원격 컴퓨터의 워커 실행파일 경로
    int ssh_port;                  /// SSH 포트 (기본값: 22)

    RemoteWorkerConfig() : ssh_port(22) {}

    RemoteWorkerConfig(const std::string& ip, const std::string& user,
        const std::string& workerpath, int port = 22)
        : ip_address(ip), username(user), worker_path(workerpath), ssh_port(port) {
    }
};

/// 설정 파일 관리 클래스
/// Master 설정 정보와 원격 워커 목록을 관리
class ConfigManager {
public:
    /// 원격 워커 설정 및 실행
    static void setupRemoteWorkers(const std::wstring& config_file) {
        /// 설정 파일이 없으면 기본 설정 파일 생성
        std::ifstream test_file(config_file);
        if (!test_file.is_open()) {
            ConfigManager::createDefaultConfig(config_file);
        }
        test_file.clear();

        /// 원격 워커 설정 로드
        auto remote_configs = ConfigManager::loadRemoteWorkers(config_file);

        if (!remote_configs.empty()) {
            /// Master의 IP 주소 자동 감지 (간단한 방법)
            std::string master_ip = getLocalIPAddress();

            /*
            /// 원격 워커 관리자 생성 및 시작
            remote_manager = std::make_unique<RemoteWorkerManager>(
                remote_configs, master_ip, port);

            std::wcout << L"Starting remote workers (Master IP: "
                << master_ip.c_str() << L":" << port << L")...\n";
            remote_manager->startRemoteWorkers();
            */

            /// 원격 워커들이 시작될 시간을 확보
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        else {
            std::wcout << L"No remote workers configured. Running with local workers only.\n";
        }
    }

private:
    /// 설정 파일에서 원격 워커 목록 로드
    /// 파일 형식: ip_address,username,worker_path,ssh_port (한 줄당 하나의 워커)
    /// 예시: 192.168.1.100,user1,/home/user1/worker,22
    static std::vector<RemoteWorkerConfig> loadRemoteWorkers(const std::wstring& config_file) {
        std::vector<RemoteWorkerConfig> workers;
        std::ifstream file(config_file);

        if (!file.is_open()) {
            std::wcout << L"Config file not found: " << config_file.c_str()
                << L". Running with local workers only.\n";
            return workers;
        }

        std::string line;
        while (std::getline(file, line)) {
            /// 빈 줄이나 주석(#으로 시작) 건너뛰기
            if (line.empty() || line[0] == '#') continue;

            /// CSV 형식 파싱
            std::vector<std::string> tokens = split(line, ',');
            if (tokens.size() >= 3) {
                RemoteWorkerConfig config;
                config.ip_address = trim(tokens[0]);
                config.username = trim(tokens[1]);
                config.worker_path = trim(tokens[2]);

                if (tokens.size() >= 4) {
                    config.ssh_port = std::stoi(trim(tokens[3]));
                }

                workers.push_back(config);
                std::wcout << L"Loaded remote worker: " << config.ip_address.c_str()
                    << L" (" << config.username.c_str() << L")\n";
            }
        }

        return workers;
    }

    /// 기본 설정 파일 생성
    static void createDefaultConfig(const std::wstring& config_file) {
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

    /// 로컬 IP 주소 가져오기
    static std::string getLocalIPAddress() {
        std::string local_ip = "127.0.0.1";

#ifdef _WIN32
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) != 0) {
            return local_ip;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET; // IPv4
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* result = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || result == nullptr) {
            return local_ip;
        }

        char ipStr[INET_ADDRSTRLEN] = { 0 };
        sockaddr_in* sockaddr_ipv4 = reinterpret_cast<sockaddr_in*>(result->ai_addr);
        inet_ntop(AF_INET, &(sockaddr_ipv4->sin_addr), ipStr, sizeof(ipStr));

        local_ip = ipStr;
        freeaddrinfo(result);
#else
        // Linux, macOS: getifaddrs() 이용
#include <ifaddrs.h>
#include <netdb.h>

        struct ifaddrs* ifaddr;
        if (getifaddrs(&ifaddr) == -1) {
            return local_ip;
        }

        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;

            if (ifa->ifa_addr->sa_family == AF_INET &&
                !(ifa->ifa_flags & IFF_LOOPBACK)) {  // 루프백 제외
                char ip[INET_ADDRSTRLEN];
                void* addr_ptr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                if (inet_ntop(AF_INET, addr_ptr, ip, sizeof(ip))) {
                    local_ip = ip;
                    break;
                }
            }
        }
        freeifaddrs(ifaddr);
#endif

        return local_ip;
    }

    /// 문자열을 구분자로 분할
    static std::vector<std::string> split(const std::string& str, char delimiter) {
        std::vector<std::string> tokens;
        std::stringstream ss(str);
        std::string token;

        while (std::getline(ss, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }

    /// 문자열 앞뒤 공백 제거
    static std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of(' ');
        if (first == std::string::npos) return "";

        size_t last = str.find_last_not_of(' ');
        return str.substr(first, (last - first + 1));
    }
};

/// 원격 워커 실행 관리 클래스
/// SSH를 통해 원격 컴퓨터의 워커를 실행하고 관리
class RemoteWorkerManager {
private:
    std::vector<RemoteWorkerConfig> remote_configs;
    std::vector<std::thread> remote_threads;
    std::string master_ip;
    int master_port;
    std::atomic<bool> should_stop;

public:
    RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
        const std::string& master_ip, int master_port)
        : remote_configs(configs), master_ip(master_ip), master_port(master_port),
        should_stop(false) {
    }

    ~RemoteWorkerManager() {
        stop();
    }

    /// 모든 원격 워커 실행 시작
    void startRemoteWorkers() {
        for (const auto& config : remote_configs) {
            remote_threads.emplace_back([this, config]() {
                this->runRemoteWorker(config);
                });
        }

        std::wcout << L"Started " << remote_configs.size() << L" remote workers\n";
    }

    /// 모든 원격 워커 중지
    void stop() {
        should_stop.store(true);

        /// 모든 원격 워커 스레드 종료 대기
        for (auto& thread : remote_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        remote_threads.clear();
    }

private:
    /// 개별 원격 워커 실행
    void runRemoteWorker(const RemoteWorkerConfig& config) {
        std::wcout << L"Starting remote worker on " << config.ip_address.c_str() << L"\n";

        /// SSH 명령어 구성
        std::stringstream ssh_command;

#ifdef _WIN32
        /// Windows에서는 putty의 plink 또는 OpenSSH 사용
        ssh_command << "ssh -p " << config.ssh_port
            << " " << config.username << "@" << config.ip_address
            << " \"" << config.worker_path
            << " worker " << master_ip << " " << master_port << "\"";
#else
        /// Linux/Unix에서는 기본 ssh 사용
        ssh_command << "ssh -p " << config.ssh_port
            << " " << config.username << "@" << config.ip_address
            << " '" << config.worker_path
            << " worker " << master_ip << " " << master_port << "'";
#endif

        std::string command = ssh_command.str();
        std::wcout << L"Executing: " << command.c_str() << L"\n";

        /// 워커가 중지될 때까지 계속 재시작 시도
        while (!should_stop.load()) {
            /// SSH를 통해 원격 워커 실행
            int result = std::system(command.c_str());

            if (result == 0) {
                std::wcout << L"Remote worker on " << config.ip_address.c_str()
                    << L" completed successfully\n";
            }
            else {
                std::wcout << L"Remote worker on " << config.ip_address.c_str()
                    << L" failed or disconnected (code: " << result << L")\n";
            }

            /// 재연결 대기 (5초)
            if (!should_stop.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }

        std::wcout << L"Remote worker thread for " << config.ip_address.c_str()
            << L" terminated\n";
    }
};

struct Point3D {
    float x, y, z;  /// x, y, z 좌표

    /// 기본 생성자 (원점으로 초기화)
    Point3D(float x = 0, float y = 0, float z = 0) : x(x), y(y), z(z) {}
};

/// Point Cloud 데이터를 청크 단위로 관리하는 구조체
/// 분산 처리를 위해 전체 데이터를 작은 단위로 나눈 것
struct PointCloudChunk {
    int chunk_id;                    /// 청크의 고유 식별자
    std::vector<Point3D> points;     /// 해당 청크에 포함된 점들

    /// 청크 데이터를 문자열로 직렬화
    /// 네트워크 전송을 위해 바이너리 데이터를 텍스트로 변환
    std::string serialize() const {
        std::stringstream ss;
        ss << chunk_id << " " << points.size() << " ";
        for (const auto& p : points) {
            ss << p.x << " " << p.y << " " << p.z << " ";
        }
        return ss.str();
    }

    /// 문자열에서 청크 데이터로 역직렬화
    /// 네트워크에서 받은 텍스트 데이터를 객체로 복원
    static PointCloudChunk deserialize(const std::string& data) {
        PointCloudChunk chunk;
        std::stringstream ss(data);
        size_t size;
        ss >> chunk.chunk_id >> size;

        chunk.points.reserve(size);  /// 메모리 효율성을 위한 사전 할당
        for (size_t i = 0; i < size; ++i) {
            Point3D p;
            ss >> p.x >> p.y >> p.z;
            chunk.points.push_back(p);
        }
        return chunk;
    }
};

/// 청크 처리 결과를 담는 구조체
struct ProcessResult {
    int chunk_id;           /// 처리된 청크의 ID
    float avg_distance;     /// 원점으로부터의 평균 거리
    int point_count;        /// 처리된 점의 개수

    /// 결과를 문자열로 직렬화
    std::string serialize() const {
        std::stringstream ss;
        ss << chunk_id << " " << avg_distance << " " << point_count;
        return ss.str();
    }

    /// 문자열에서 결과 객체로 역직렬화
    static ProcessResult deserialize(const std::string& data) {
        ProcessResult result;
        std::stringstream ss(data);
        ss >> result.chunk_id >> result.avg_distance >> result.point_count;
        return result;
    }
};

/// Windows Winsock 초기화를 RAII 패턴으로 관리하는 클래스
/// 프로그램 시작 시 자동 초기화, 종료 시 자동 정리
class WinsockInitializer {
public:
    WinsockInitializer() {
#ifdef _WIN32
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup failed: " << result << std::endl;
            initialized = false;
        }
        else {
            initialized = true;
        }
#else
        /// Linux에서는 별도 초기화 불필요
        initialized = true;
#endif
    }

    ~WinsockInitializer() {
#ifdef _WIN32
        if (initialized) {
            WSACleanup();
        }
#endif
    }

    bool isInitialized() const { return initialized; }

private:
    bool initialized;
};

/// 네트워크 통신을 위한 유틸리티 클래스
/// TCP 소켓을 통한 안전한 데이터 송수신 기능 제공
class NetworkUtils {
public:
    /// 소켓을 통해 데이터를 안전하게 전송
    /// 먼저 데이터 크기를 보낸 후 실제 데이터 전송 (프로토콜)
    static bool sendData(SOCKET socket, const std::string& data) {
        uint32_t size = static_cast<uint32_t>(data.length());

        /// 크기 전송 (4바이트 고정)
        if (send(socket, reinterpret_cast<const char*>(&size), sizeof(size), 0) != sizeof(size)) {
            return false;
        }

        /// 데이터 전송 (부분 전송 가능성을 고려한 루프)
        const char* buffer = data.c_str();
        int totalSent = 0;
        int dataSize = static_cast<int>(size);

        while (totalSent < dataSize) {
            int sent = send(socket, buffer + totalSent, dataSize - totalSent, 0);
            if (sent == SOCKET_ERROR) {
                return false;
            }
            totalSent += sent;
        }
        return true;
    }

    /// 소켓에서 데이터를 안전하게 수신
    /// 먼저 데이터 크기를 받은 후 해당 크기만큼 데이터 수신
    static std::string receiveData(SOCKET socket) {
        uint32_t size;

        /// 크기 수신 (4바이트 고정)
        int received = recv(socket, reinterpret_cast<char*>(&size), sizeof(size), 0);
        if (received != sizeof(size)) {
            return "";
        }

        /// 데이터 수신 (부분 수신 가능성을 고려한 루프)
        std::string data(size, '\0');
        int totalReceived = 0;
        int dataSize = static_cast<int>(size);

        while (totalReceived < dataSize) {
            int received = recv(socket, &data[totalReceived], dataSize - totalReceived, 0);
            if (received == SOCKET_ERROR || received == 0) {
                return "";
            }
            totalReceived += received;
        }

        return data;
    }

    /// 워커에게 작업 종료 신호 전송
    static bool sendTerminationSignal(SOCKET socket) {
        std::string terminationSignal = "TERMINATE";
        return sendData(socket, terminationSignal);
    }

    /// 수신한 데이터가 종료 신호인지 확인
    static bool isTerminationSignal(const std::string& data) {
        return data == "TERMINATE";
    }
};

/// Master 서버 클래스
/// Point Cloud 데이터를 생성하고 워커들에게 작업을 분배
/// 원격 워커 자동 실행 기능 포함
class MasterServer {
private:
    std::vector<PointCloudChunk> chunks;                    /// 처리할 청크들
    std::vector<ProcessResult> results;                     /// 처리 결과들
    std::atomic<int> chunk_index;                           /// 다음 할당할 청크 인덱스 (원자적 연산)
    std::mutex results_mutex;                               /// 결과 벡터 접근 동기화
    int port;                                               /// 서버 포트 번호
    std::atomic<bool> all_chunks_processed;                 /// 모든 청크 처리 완료 플래그
    std::unique_ptr<RemoteWorkerManager> remote_manager;    /// 원격 워커 관리자
    std::wstring config_file;                                /// 설정 파일 경로

    std::vector<std::vector<std::vector<Face>>> allFileMeshes;
    std::vector<int> allFileBimId;

public:
    MasterServer(int p, const std::wstring& config = L"workers.conf")
        : port(p), chunk_index(0), all_chunks_processed(false), config_file(config) {
    }

    ~MasterServer() {
        /// 원격 워커 정리
        if (remote_manager) {
            remote_manager->stop();
        }
    }

    /// 테스트용 샘플 Point Cloud 데이터 생성
    /// 실제 환경에서는 파일에서 로드하거나 센서에서 입력받을 수 있음
    void generateSampleData() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> dis(-100.0f, 100.0f);

        /// 10개의 청크로 분할된 샘플 Point Cloud 생성
        for (int chunk_id = 0; chunk_id < 10; ++chunk_id) {
            PointCloudChunk chunk;
            chunk.chunk_id = chunk_id;

            /// 각 청크마다 1000개의 포인트 생성
            for (int i = 0; i < 1000; ++i) {
                chunk.points.emplace_back(dis(gen), dis(gen), dis(gen));
            }
            chunks.push_back(chunk);
        }
        std::wcout << L"Generated " << chunks.size() << L" chunks with total "
            << chunks.size() * 1000 << L" points\n";
    }

    /// Master 서버 시작
    /// 원격 워커 자동 실행 후 TCP 서버를 시작하고 워커들의 연결을 처리
    void start() {
        /// 원격 워커 설정 로드 및 실행
        ConfigManager::setupRemoteWorkers(config_file);

        /// TCP 소켓 생성
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock == INVALID_SOCKET) {
            std::wcerr << L"Socket creation failed\n";
            return;
        }

        /// SO_REUSEADDR 설정으로 주소 재사용 허용
        int opt = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&opt), sizeof(opt));

        /// 서버 주소 설정
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;  /// 모든 인터페이스에서 수신
        addr.sin_port = htons(port);

        /// 소켓을 주소에 바인딩
        if (bind(server_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::wcerr << L"Bind failed\n";
            close(server_sock);
            return;
        }

        /// 연결 대기 상태로 전환
        if (listen(server_sock, 10) == SOCKET_ERROR) {
            std::wcerr << L"Listen failed\n";
            close(server_sock);
            return;
        }

        std::wcout << L"Master server listening on port " << port << L"\n";

        /// 모든 청크가 처리될 때까지 워커 연결 처리
        while (!all_chunks_processed.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);

            /// 워커의 연결 수락
            SOCKET client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

            if (client_sock == INVALID_SOCKET) continue;

            /// 연결된 워커의 IP 주소 출력
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, INET_ADDRSTRLEN);
            std::wcout << L"Worker connected: " << ipStr << L"\n";

            /// 처리할 청크가 남아있는지 확인 (원자적 연산)
            int current_chunk = chunk_index.fetch_add(1);
            if (current_chunk < static_cast<int>(chunks.size())) {
                /// 청크 전송
                std::string chunk_data = chunks[current_chunk].serialize();
                if (NetworkUtils::sendData(client_sock, chunk_data)) {
                    std::wcout << L"Sent chunk " << current_chunk << L" to worker\n";

                    /// 결과 수신
                    std::string result_data = NetworkUtils::receiveData(client_sock);
                    if (!result_data.empty()) {
                        ProcessResult result = ProcessResult::deserialize(result_data);

                        /// 결과 저장 (스레드 안전)
                        {
                            std::lock_guard<std::mutex> lock(results_mutex);
                            results.push_back(result);
                            std::wcout << L"Received result for chunk " << result.chunk_id
                                << L": avg_distance=" << result.avg_distance
                                << L", points=" << result.point_count << L"\n";

                            /// 모든 청크가 처리되었는지 확인
                            if (results.size() == chunks.size()) {
                                all_chunks_processed.store(true);
                                std::wcout << L"All chunks processed!\n";
                            }
                        }
                    }
                }
            }
            else {
                /// 더 이상 처리할 청크가 없음을 알림
                NetworkUtils::sendTerminationSignal(client_sock);
                std::wcout << L"Sent termination signal to worker\n";
            }

            close(client_sock);
        }

        close(server_sock);
        printFinalResults();

        /// 원격 워커들 정리
        if (remote_manager) {
            std::wcout << L"Shutting down remote workers...\n";
            remote_manager->stop();
        }
    }

    void startGltf() {
        /// 원격 워커 설정 로드 및 실행
        ConfigManager::setupRemoteWorkers(config_file);

        /// TCP 소켓 생성
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock == INVALID_SOCKET) {
            std::wcerr << L"Socket creation failed\n";
            return;
        }

        /// SO_REUSEADDR 설정으로 주소 재사용 허용
        int opt = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&opt), sizeof(opt));

        /// 서버 주소 설정
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;  /// 모든 인터페이스에서 수신
        addr.sin_port = htons(port);

        /// 소켓을 주소에 바인딩
        if (bind(server_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::wcerr << L"Bind failed\n";
            close(server_sock);
            return;
        }

        /// 연결 대기 상태로 전환
        if (listen(server_sock, 10) == SOCKET_ERROR) {
            std::wcerr << L"Listen failed\n";
            close(server_sock);
            return;
        }

        std::wcout << L"Master server listening on port " << port << L"\n";

        size_t numProcessedMeshes = 0;
        size_t numAllMeshes = 0;
        for (const auto meshes : this->allFileMeshes) {
            numAllMeshes += meshes.size();
        }

        while (numProcessedMeshes < numAllMeshes) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);

            /// 워커의 연결 수락
            SOCKET client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

            if (client_sock == INVALID_SOCKET) continue;

            /// 연결된 워커의 IP 주소 출력
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, INET_ADDRSTRLEN);
            std::wcout << L"Worker connected: " << ipStr << L"\n";

            /// 처리할 청크가 남아있는지 확인 (원자적 연산: 다른 스레드가 동시에 접속해도 연산이 중단되지 않고 한 번에 완료, data race condition 회피)
            /// current_chunk에 마지막 chunk_index를 대입하고, cuunk_index에 '1'을 추가
            int current_chunk = chunk_index.fetch_add(1);
            if (current_chunk < static_cast<int>(numAllMeshes)) {
                /// 청크 전송
                std::string chunk_data = chunks[current_chunk].serialize();
                if (NetworkUtils::sendData(client_sock, chunk_data)) {
                    std::wcout << L"Sent chunk " << current_chunk << L" to worker\n";

                    /// 결과 수신
                    std::string result_data = NetworkUtils::receiveData(client_sock);
                    if (!result_data.empty()) {
                        ProcessResult result = ProcessResult::deserialize(result_data);

                        /// 결과 저장 (스레드 안전)
                        {
                            std::lock_guard<std::mutex> lock(results_mutex);
                            results.push_back(result);
                            std::wcout << L"Received result for chunk " << result.chunk_id
                                << L": avg_distance=" << result.avg_distance
                                << L", points=" << result.point_count << L"\n";

                            /// 모든 청크가 처리되었는지 확인
                            if (results.size() == chunks.size()) {
                                all_chunks_processed.store(true);
                                std::wcout << L"All chunks processed!\n";
                            }
                        }
                    }
                }
            }
            else {
                /// 더 이상 처리할 청크가 없음을 알림
                NetworkUtils::sendTerminationSignal(client_sock);
                std::wcout << L"Sent termination signal to worker\n";
            }

            ++numProcessedMeshes;

            close(client_sock);
        }

        /// 모든 청크가 처리될 때까지 워커 연결 처리
        while (!all_chunks_processed.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);

            /// 워커의 연결 수락
            SOCKET client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

            if (client_sock == INVALID_SOCKET) continue;

            /// 연결된 워커의 IP 주소 출력
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, INET_ADDRSTRLEN);
            std::wcout << L"Worker connected: " << ipStr << L"\n";

            /// 처리할 청크가 남아있는지 확인 (원자적 연산)
            int current_chunk = chunk_index.fetch_add(1);
            if (current_chunk < static_cast<int>(chunks.size())) {
                /// 청크 전송
                std::string chunk_data = chunks[current_chunk].serialize();
                if (NetworkUtils::sendData(client_sock, chunk_data)) {
                    std::wcout << L"Sent chunk " << current_chunk << L" to worker\n";

                    /// 결과 수신
                    std::string result_data = NetworkUtils::receiveData(client_sock);
                    if (!result_data.empty()) {
                        ProcessResult result = ProcessResult::deserialize(result_data);

                        /// 결과 저장 (스레드 안전)
                        {
                            std::lock_guard<std::mutex> lock(results_mutex);
                            results.push_back(result);
                            std::wcout << L"Received result for chunk " << result.chunk_id
                                << L": avg_distance=" << result.avg_distance
                                << L", points=" << result.point_count << L"\n";

                            /// 모든 청크가 처리되었는지 확인
                            if (results.size() == chunks.size()) {
                                all_chunks_processed.store(true);
                                std::wcout << L"All chunks processed!\n";
                            }
                        }
                    }
                }
            }
            else {
                /// 더 이상 처리할 청크가 없음을 알림
                NetworkUtils::sendTerminationSignal(client_sock);
                std::wcout << L"Sent termination signal to worker\n";
            }

            close(client_sock);
        }

        close(server_sock);
        printFinalResults();

        /// 원격 워커들 정리
        if (remote_manager) {
            std::wcout << L"Shutting down remote workers...\n";
            remote_manager->stop();
        }
    }

    bool readInputFolder(const std::string& project_name,
        const std::string& bim_folder,
        const std::string& nodes2_folder,
        const bool is_offset_applied,
        const float* offset,
        const float unitConversion = FEET_TO_METER) {

		auto split = [](const std::string& str, const char delimiter) {
            std::vector<std::string> parts;
            std::stringstream ss(str);
            std::string item;

            while (std::getline(ss, item, delimiter)) {
                parts.push_back(item);
            }

            return parts;
        };

        fs::path input_path = bim_folder; /// model 파일이 저장된 폴더 경로
        fs::path output_path = nodes2_folder; // .nodes2 파일을 저장할 폴더 경로

        if (std::filesystem::exists(input_path) == false) {
            std::cout << "input_path '" << input_path << "' is not exist" << std::endl;
            return false;
        }

        size_t total_gltf_files = std::distance(fs::directory_iterator(input_path), fs::directory_iterator());
        int processed_gltf_files = 0;

        allFileMeshes.clear();
        allFileBimId.clear();

        std::string output_file_name = project_name + ".nodes2"; // binary file name
        auto full_path = output_path / output_file_name;
        output_file_name = full_path.string();

        int idx = 1000; // bim_id의 초기값 - 파일명에 Revit Element ID가 없는 경우 사용됨
        int nFile = 0;
        
        for (const auto& entry : std::filesystem::directory_iterator(input_path))
        {
            ++processed_gltf_files;

            std::cout << entry.path().string() << std::endl; /// utf8 string
            if (!entry.is_regular_file()) {
                std::cout << entry.path().string() << " not regular file" << std::endl; /// utf8 string
                continue;
            }

            auto& data_file = entry.path();
            if (data_file.extension().compare(".gltf") != 0 && data_file.extension().compare(".glb") != 0) {
                if (data_file.extension().compare(".bin") != 0)
                    std::cout << data_file << " not gltf(glb) file" << std::endl;
                continue;
            }

            /// UTF-16을 UTF-8로 변환
            std::string gltf_file_name = wstring_to_utf8(data_file);

            allFileMeshes.emplace_back();
            allFileBimId.emplace_back(-1);
            auto& currentMeshes = allFileMeshes.back();
            auto& currentBimId = allFileBimId.back();

            /// create new gltf_file object
            if (readInputFile(gltf_file_name, currentMeshes, unitConversion)) {
                std::cerr << "Failure in reading " << gltf_file_name << std::endl;
                return false;
            }

            /// bim_id, node_name 설정
            int bim_id = idx;
            std::string node_name;
            try {
                /// 파일 이름에서 확장자를 제외하고 '_'로 분할하여 마지막 문자열을 bim_id로 설정합니다.
                std::wstring filenameW = data_file.stem().wstring(); // 파일 이름 - convert to unicode
                std::string filename = wstring_to_utf8(filenameW);
                std::vector<std::string> parts = split(filename, '_');

                /// 문자열의 마지막 부분(Revit Element ID)를 bim_id로 변환합니다.
                if (!parts.empty()) {
                    std::string lastPart = parts.back();
                    /// 숫자만 남기기 위해 문자 제거
                    lastPart.erase(std::remove_if(lastPart.begin(), lastPart.end(), [](char c) { return !std::isdigit(c); }), lastPart.end());
                    if (!lastPart.empty()) {
                        bim_id = std::stoi(lastPart);
                    }
                    else {
                        std::cerr << "Error: No numeric part found in the last part of filename" << std::endl;
                        bim_id = ++idx; /// 기본값(1,000)에서 증가시킴
                    }
                }
                else {
                    std::cerr << "Error: Filename does not contain valid parts" << std::endl;
                    bim_id = ++idx; /// 기본값(1,000)에서 증가시킴
                }

                // 마지막 '_'의 위치를 찾고 앞부분(node name)을 가져옵니다.
                size_t lastUnderscorePos = filename.find_last_of("_");
                node_name = filename.substr(0, lastUnderscorePos); // node name 
            }
            catch (const std::exception& e) {
                std::cerr << data_file.stem() << std::endl;
                std::cerr << e.what() << std::endl;
                bim_id = ++idx;
            }

            currentBimId = bim_id;
            ++nFile;            

            // Progress 출력
            int progress = (int)((float)processed_gltf_files / total_gltf_files * 100);
            if (processed_gltf_files % 100 == 0 || progress == 100)
                std::cout << "Processed " << processed_gltf_files << " / " << total_gltf_files << " (" << progress << "%)" << std::endl;
        }

        return true;
    }

    bool readInputFile(const std::string fileName, std::vector<std::vector<Face>>& meshes, const float unitConversion)
    {
	    tinygltf::Model model;
	    tinygltf::TinyGLTF loader;
	    std::string err;
	    std::string warn;

	    bool fileread = false;

	    if (fileName.substr(fileName.find_last_of(".") + 1) == "glb") {
		    fileread = loader.LoadBinaryFromFile(&model, &err, &warn, fileName);
	    }
	    else if (fileName.substr(fileName.find_last_of(".") + 1) == "gltf") {
		    fileread = loader.LoadASCIIFromFile(&model, &err, &warn, fileName);
	    }
	    else {
		    std::cerr << "Unsupported file format" << std::endl;
            return false;
	    }

	    if (!warn.empty()) {
		    std::cerr << "Warning: " << warn << std::endl;
	    }

	    if (!err.empty()) {
		    std::cerr << "Error: " << err << std::endl;
	    }

	    if (!fileread) {
		    std::cerr << "Failed to parse glTF, "<< fileName << std::endl;
            return false;
	    }

	    // Access mesh information
        std::vector<size_t> numTriangles(model.meshes.size());
#ifdef _DEBUG
        std::clog << "Number of meshes: " << model.meshes.size() << std::endl;
#endif
	    for (size_t i = 0; i < model.meshes.size(); ++i) {
		    const tinygltf::Mesh& mesh = model.meshes[i];
            size_t nTri = 0;
#ifdef _DEBUG
            std::clog << i << "_th mesh " << mesh.name << "\t";
            std::clog << "Number of primitives: " << mesh.primitives.size() << std::endl;
            if (mesh.primitives.size() > 1) {
                std::clog << i << "_th mesh \t";
                std::clog << "has more than 1 primitives: " << std::endl;
            }
#endif
		    // Process each primitive in the mesh
            std::vector<Face> faces;
            for (size_t j = 0; j < mesh.primitives.size(); ++j) {
                const tinygltf::Primitive& primitive = mesh.primitives[j];

                // Access primitive attributes
                const auto& attributes = primitive.attributes;

                // Check if attributes contain POSITION and NORMAL
                if (attributes.find("POSITION") == attributes.end() || attributes.find("NORMAL") == attributes.end()) {
                    std::cerr << "Missing required attributes (POSITION and/or NORMAL)" << std::endl;
                    continue;
                }

                // Access position and normal indices
                int positionIdx = primitive.attributes.at("POSITION"); // error: no operator "[]" matches these operands
                int normalIdx = primitive.attributes.at("NORMAL");

                // Access index buffer view
                const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
                const tinygltf::BufferView& indexBufferView = model.bufferViews[indexAccessor.bufferView];
                const tinygltf::Buffer& indexBuffer = model.buffers[indexBufferView.buffer];
                const uint16_t* indicesPtr = reinterpret_cast<const uint16_t*>(&indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset]);
                const size_t numIndices = indexAccessor.count;

                // Access position and normal buffers
                const tinygltf::Accessor& positionAccessor = model.accessors[positionIdx];
                const tinygltf::Accessor& normalAccessor = model.accessors[normalIdx];
                const tinygltf::BufferView& positionBufferView = model.bufferViews[positionAccessor.bufferView];
                const tinygltf::BufferView& normalBufferView = model.bufferViews[normalAccessor.bufferView];
                const tinygltf::Buffer& positionBuffer = model.buffers[positionBufferView.buffer];
                const tinygltf::Buffer& normalBuffer = model.buffers[normalBufferView.buffer];
                const float* positions = reinterpret_cast<const float*>(&(positionBuffer.data[positionBufferView.byteOffset + positionAccessor.byteOffset]));
                const float* normals = reinterpret_cast<const float*>(&(normalBuffer.data[normalBufferView.byteOffset + normalAccessor.byteOffset]));
                size_t numVertices = positionAccessor.count;
                size_t numNormals = normalAccessor.count;

                size_t numTriangles = numIndices / 3;
                nTri += numTriangles;
#ifdef _DEBUG
                std::clog << "Number of triangles: " << numTriangles << std::endl;
#endif

                // Iterate over the indices to create triangles
                for (int k = 0; k < numIndices; k += 3) {
                    /// Extract vertex indices for the triangle
                    int idx1 = static_cast<int>(indicesPtr[k]);
                    int idx2 = static_cast<int>(indicesPtr[k + 1]);
                    int idx3 = static_cast<int>(indicesPtr[k + 2]);

                    /// Unit conversion (ex, FEET_TO_METER 변환)
                    Point v1, v2, v3;
                    if (unitConversion != 1.0f) {
                        v1 = Point(positions[idx1 * 3] * unitConversion, positions[idx1 * 3 + 1] * unitConversion, positions[idx1 * 3 + 2] * unitConversion);
                        v2 = Point(positions[idx2 * 3] * unitConversion, positions[idx2 * 3 + 1] * unitConversion, positions[idx2 * 3 + 2] * unitConversion);
                        v3 = Point(positions[idx3 * 3] * unitConversion, positions[idx3 * 3 + 1] * unitConversion, positions[idx3 * 3 + 2] * unitConversion);
                    }
                    else {
                        v1 = Point(positions[idx1 * 3], positions[idx1 * 3 + 1], positions[idx1 * 3 + 2]);
                        v2 = Point(positions[idx2 * 3], positions[idx2 * 3 + 1], positions[idx2 * 3 + 2]);
                        v3 = Point(positions[idx3 * 3], positions[idx3 * 3 + 1], positions[idx3 * 3 + 2]);
                    }

                    Point n1(normals[idx1 * 3], normals[idx1 * 3 + 1], normals[idx1 * 3 + 2]);

                    // Add facce
                    faces.emplace_back(n1, v1, v2, v3);
                }
            }
            
            /// add a mesh
            meshes.emplace_back(faces);
	    }

	    return true;
    }

private:
    /// 모든 처리가 완료된 후 최종 결과 출력
    void printFinalResults() {
        std::wcout << L"\n=== Final Results ===\n";
        float total_avg = 0.0f;
        int total_points = 0;

        /// 각 청크별 결과 출력
        for (const auto& result : results) {
            total_avg += result.avg_distance;
            total_points += result.point_count;
            std::wcout << L"Chunk " << result.chunk_id << L": "
                << result.point_count << L" points, avg_distance="
                << result.avg_distance << L"\n";
        }

        /// 전체 통계 출력
        if (!results.empty()) {
            std::wcout << L"Overall average distance: " << total_avg / results.size() << L"\n";
            std::wcout << L"Total points processed: " << total_points << L"\n";
        }
    }
};

/// Worker 클라이언트 클래스
/// Master에 연결하여 Point Cloud 청크를 처리
class WorkerClient {
private:
    std::string master_ip;      /// Master 서버의 IP 주소
    int master_port;            /// Master 서버의 포트 번호

public:
    WorkerClient(const std::string& ip, int port) : master_ip(ip), master_port(port) {}

    /// Worker 시작
    /// Master의 모든 데이터가 처리될 때까지 계속 연결하여 작업 수행
    void start() {
        std::wcout << L"Worker started. Connecting to master...\n";

        /// Master의 모든 데이터가 처리될 때까지 계속 연결
        while (true) {
            if (!connectAndProcess()) {
                std::wcout << L"Worker terminated.\n";
                break;
            }

            /// 다음 연결 전 잠시 대기 (서버 부하 감소)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

private:
    /// Master에 연결하여 한 번의 작업 처리
    /// 성공 시 true, 종료 신호 수신 시 false 반환
    bool connectAndProcess() {
        /// TCP 소켓 생성
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) {
            std::wcerr << L"Socket creation failed\n";
            return false;
        }

        /// Master 서버 주소 설정
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(master_port);

        /// IP 주소 변환 및 검증
        if (InetPtonA(AF_INET, master_ip.c_str(), &addr.sin_addr) != 1) {
            std::wcerr << L"Invalid IP address\n";
            close(sock);
            return false;
        }

        if (addr.sin_addr.s_addr == INADDR_NONE) {
            std::wcerr << L"Invalid IP address\n";
            close(sock);
            return false;
        }

        /// Master 서버에 연결
        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::wcerr << L"Connection to master failed\n";
            close(sock);
            return false;
        }

        /// 청크 또는 종료 신호 수신
        std::string received_data = NetworkUtils::receiveData(sock);
        if (received_data.empty()) {
            std::wcerr << L"Failed to receive data from master\n";
            close(sock);
            return false;
        }

        /// 종료 신호 확인
        if (NetworkUtils::isTerminationSignal(received_data)) {
            std::wcout << L"Received termination signal from master\n";
            close(sock);
            return false; // 워커 종료
        }

        /// 청크 데이터 처리
        PointCloudChunk chunk = PointCloudChunk::deserialize(received_data);
        std::wcout << L"Received chunk " << chunk.chunk_id
            << L" with " << chunk.points.size() << L" points\n";

        /// Point Cloud 처리 수행
        ProcessResult result = processPointCloud(chunk);

        /// 결과 전송
        std::string result_data = result.serialize();
        NetworkUtils::sendData(sock, result_data);
        std::wcout << L"Sent processing result for chunk " << result.chunk_id << L"\n";

        close(sock);
        return true; // 계속 실행
    }

    /// Point Cloud 청크 처리 함수
    /// 각 점의 원점으로부터의 거리를 계산하여 평균 구함
    ProcessResult processPointCloud(const PointCloudChunk& chunk) {
        std::wcout << L"Processing chunk " << chunk.chunk_id << L"...\n";

        /// 시뮬레이션: 원점으로부터의 평균 거리 계산
        float total_distance = 0.0f;
        for (const auto& point : chunk.points) {
            /// 유클리드 거리 계산 (√(x² + y² + z²))
            float distance = std::sqrt(point.x * point.x + point.y * point.y + point.z * point.z);
            total_distance += distance;
        }

        /// 처리 시간 시뮬레이션 (실제 복잡한 계산 대신)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        /// 결과 객체 생성
        ProcessResult result;
        result.chunk_id = chunk.chunk_id;
        result.avg_distance = total_distance / chunk.points.size();
        result.point_count = static_cast<int>(chunk.points.size());

        std::wcout << L"Finished processing chunk " << chunk.chunk_id
            << L" (avg_distance: " << result.avg_distance << ")\n";

        return result;
    }
};

/// 프로그램 진입점 (유니코드 지원)
int wmain(int argc, wchar_t* argv[]) {
    /// 콘솔 출력을 유니코드로 설정 (윈도우 전용)
    auto retval = _setmode(_fileno(stdout), _O_U16TEXT);

    /// Winsock 초기화 (윈도우 전용)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::wcerr << L"WSAStartup 실패\n";
        return 1;
    }

    /// 명령행 인수 검증
    if (argc < 2) {
        std::wcout << L"Usage:\n";
        std::wcout << L"  Master mode: " << argv[0] << L" master [port]\n";
        std::wcout << L"  Worker mode: " << argv[0] << L" worker [master_ip] [master_port]\n";
        return 1;
    }

    std::wstring mode = argv[1];

    /// Master 모드 실행
    if (mode == L"master") {
        int port = (argc >= 3) ? _wtoi(argv[2]) : 8080;
        std::wstring config_file = (argc >= 4) ? argv[3] : L"workers.conf";

        MasterServer server(port, config_file);
        //server.generateSampleData();  /// 테스트 데이터 생성
        const std::string glbFolder = "C:\\Users\\kiinb\\Downloads\\samsung_test\\glb2";
        const std::string nodes2_folder = "C:\\Users\\kiinb\\Downloads\\samsung_test\\nodes2";
        float offset[3] = { 1.0f, 1.0f, 1.0f };
        const float unitConversion = FEET_TO_METER; /// set this value to config later
        server.readInputFolder("samsung_test", glbFolder, nodes2_folder, false, offset, unitConversion);
        server.start();               /// 서버 시작 (원격 워커 자동 실행 포함)
    }
    /// Worker 모드 실행
    else if (mode == L"worker") {
        std::wstring master_ip = (argc >= 3) ? argv[2] : L"127.0.0.1";
        int master_port = (argc >= 4) ? _wtoi(argv[3]) : 8080;
        WorkerClient worker(wstring_to_utf8(master_ip), master_port);
        worker.start();  /// 워커 시작
    }
    else {
        std::wcerr << L"Invalid mode. Use 'master' or 'worker'\n\n";
        return 1;
    }

    /// Winsock 정리 (윈도우 전용)
    WSACleanup();
    return 0;
}