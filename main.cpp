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
/// [Q] 모든 작업완료 전에, 모든 woker가 종료되면 어떻게 되지?
/// [A] 모든 chunk가 끝나기 전에 워커가 전부 종료되면, 마스터는 accept 루프에서 계속 대기함
/// draining은 모든 결과를 다 받았을 때만 켜지므로 아직 draining으로도 못 넘어가고 멈춰 있게 됨
/// 다만 원격 워커가 설정돼 있고 RemoteWorkerManager가 돌고 있으면, 
/// 그 쓰레드가 주기적으로 다시 워커를 띄우기 때문에 곧 새 워커가 접속해 이어서 처리하게 됨
/// 로컬 워커만 가동된 경우(한 번만 새 창으로 워커 실행) : 워커가 닫히면 
/// 자동 재시작이 없어서 마스터는 새 접속이 오기 전까지 진행이 무한히 멈추게 됨 (수정될 예정)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
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

/// <windows.h> 는 <winsock2.h> 보다 먼저 포함되면 충돌
/// <windows.h> 안에는 오래된 <winsock.h> 를 암묵적으로 포함하는 경우가 있어서, 이후 <winsock2.h> 와 상수 / 매크로 / 함수가 중복 정의되어 에러가 발생
/// 따라서, #include <winsock2.h> 를 반드시 <windows.h>보다 먼저 포함해야 함
#ifdef _WIN32
#include <windows.h>
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

/// === Self executable absolute path ===
// 요구 헤더(이미 포함돼 있다면 중복 include는 생략하세요)
#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#include <vector>
#include <string>
#endif

// 문자열 유틸: 프로젝트에 이미 있는 wstring_to_utf8 사용
// std::string wstring_to_utf8(const std::wstring&);

static std::string getSelfExePath() {
#ifdef _WIN32
    // Wide 버전으로 얻고 UTF-8로 변환: 한글/비 ASCII 경로 안전
    std::wstring wpath;
    DWORD cap = 260; // 초기 버퍼(필요 시 확장)
    for (;;) {
        std::vector<wchar_t> buf(cap);
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return std::string(); // 실패
        }
        if (n < buf.size() - 1) {
            wpath.assign(buf.data(), n);
            break;
        }
        cap *= 2; // 버퍼 확장 후 재시도
    }

    // (선택) 절대 경로/롱패스 정규화가 필요하면 GetFullPathNameW/ GetLongPathNameW 추가 가능
    return wstring_to_utf8(wpath);

#elif __APPLE__
    // 기존 로직 유지 + realpath로 심볼릭 링크/상대경로 해소
#include <mach-o/dyld.h>
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::string();
    char resolved[PATH_MAX] = { 0 };
    if (realpath(buf.data(), resolved)) return std::string(resolved);
    return std::string(buf.data());  // realpath 실패 시 원본

#else
    // Linux: /proc/self/exe → 가능하면 realpath로 정규화
    std::vector<char> buf(4096);
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    char resolved[PATH_MAX] = { 0 };
    if (realpath(buf.data(), resolved)) return std::string(resolved);
    return std::string(buf.data());  // realpath 실패 시 원본
#endif
}

static int startLocalWorkerNewWindow(const std::string& exePath,
    const std::string& master_ip,
    int master_port) {
    // 공통 base 커맨드
    const std::string base = "\"" + exePath + "\" worker " + master_ip + " " + std::to_string(master_port);

#if defined(_WIN32)
    // 새 콘솔 창: cmd /c start "" "<exe>" args...
    std::string cmd = "cmd /c start \"\" " + base;
    return std::system(cmd.c_str());

#elif defined(__APPLE__)
    // Terminal.app 새 창
    std::string bashLine = "bash -lc \\\"" + escapeDQ(base) + "; exec bash -i\\\"";
    std::string cmd = "osascript -e 'tell application \"Terminal\" to do script \"" + bashLine + "\"' "
        "-e 'tell application \"Terminal\" to activate'";
    return std::system(cmd.c_str());

#else
    // Linux: gnome-terminal/konsole/xterm 우선, 없으면 tmux 백그라운드
    std::string inner =
        "if [ -z \"$DISPLAY\" ]; then export DISPLAY=:0; fi; "
        "if command -v gnome-terminal >/dev/null 2>&1; then "
        "gnome-terminal -- bash -lc \"" + escapeDQ(base) + "; exec bash -i\"; "
        "elif command -v konsole >/dev/null 2>&1; then "
        "konsole --hold -e bash -lc \"" + escapeDQ(base) + "; exec bash -i\"; "
        "elif command -v xterm >/dev/null 2>&1; then "
        "xterm -hold -e \"" + escapeDQ(base) + "\"; "
        "else "
        "tmux new-session -d -s pointcloud \"" + escapeDQ(base) + "\"; "
        "echo \"No desktop terminal found; started in tmux session 'pointcloud'\"; "
        "fi";
    std::string cmd = "bash -lc \"" + escapeDQ(inner) + "\"";
    return std::system(cmd.c_str());
#endif
}

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

    bool run_local_worker_on_master = false;

    int master_starvation_seconds = 30;   /// 진행 없음 + 워커 무접속 임계시간
    int emergency_local_spawn_max = 3;    /// 비상 로컬 워커 재기동 최대 횟수
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

        // conf 최초 생성 시 쓰는 템플릿 내용에 추가
        file << "# --- options ---\n";
        file << "run_local_worker_on_master=true\n";   /// 기본값 true
        file << "\n";

        file << "# --- workers (examples) ---\n";
        file << "# ip,username,worker_path[,ssh_port][,os]\n";
        file << "#10.10.10.17,kiinbang,C:\\Workers\\Test_master_worker.exe,22,windows\n";
        file << "#10.10.10.18,kiinbang,/Users/kiinbang/Workers/Test_master_worker,22,mac\n";
        file << "#10.10.10.19,kiinbang,/home/kiinbang/Workers/Test_master_worker,22,linux\n";
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
            std::wcout << L"Config file not found: " << utf8_to_wstring(config_file)
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
                    std::wcout << L"Loaded remote worker: " << utf8_to_wstring(cfg.ip_address)
                        << L" (" << utf8_to_wstring(cfg.username) << L")\n";
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
                else if (lk == "run_local_worker_on_master") {
                    if (lv == "1" || lv == "true") s.run_local_worker_on_master = true;
                    else s.run_local_worker_on_master = false;
                }
                else if (lk == "master_starvation_seconds") s.master_starvation_seconds = std::stoi(v);
                else if (lk == "emergency_local_spawn_max") s.emergency_local_spawn_max = std::stoi(v);
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
        std::wcout << L"Executing: " << utf8_to_wstring(cmd) << L"\n";

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
    /// 재배당 대기열 및 완료 집합
    std::deque<int> retry_queue_;
    std::mutex      retry_mtx_;
    std::unordered_set<int> completed_chunk_ids_;
    std::chrono::steady_clock::time_point last_progress_{ std::chrono::steady_clock::now() };
    int emergency_local_spawned_ = 0;

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
        for (int cid = 0; cid < 100; ++cid) {
            PointCloudChunk c; c.chunk_id = cid;
            for (int i = 0; i < 1000; ++i) c.points.emplace_back(dis(gen), dis(gen), dis(gen));
            chunks.push_back(std::move(c));
        }
        std::wcout << L"Generated " << chunks.size() << L" chunks with total "
            << chunks.size() * 1000 << L" points\n";
    }

    bool has_pending_work() const {
        /// 완료한 고유 청크 수 < 전체 청크 수 면 아직 할 일 있음
        return completed_chunk_ids_.size() < chunks.size();
    }

    /// 서버 시작:
    /// 1) (옵션) 원격 워커 자동 실행
    /// 2) TCP listen → accept 루프에서 워커에 청크 전송/결과 수신
    void start() {
        /// Load config (create default if missing)
        ConfigManager::ensureConfig(config_file);
        settings = ConfigManager::loadRuntimeSettings(config_file);
        remote_configs = ConfigManager::loadRemoteWorkers(config_file);

        /// Start a local worker
        if (settings.run_local_worker_on_master) {
            const std::string selfExe = getSelfExePath();

            int rc = startLocalWorkerNewWindow(selfExe, "127.0.0.1", this->port);
#ifdef _WIN32
            if (rc != 0) {
                std::wcerr << L"[WARN ] Local worker start failed (rc=" << rc << L")\n";
            }
            else {
                std::wcout << L"[ OK  ] Local worker started in a new window\n";
            }
#else
            if (rc != 0) {
                std::cerr << "[WARN ] Local worker start failed (rc=" << rc << ")\n";
            }
            else {
                std::cout << "[ OK  ] Local worker started in a new window\n";
            }
#endif
        }

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
                << utf8_to_wstring(master_ip) << L":" << port << L")...\n";
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
            auto now = std::chrono::steady_clock::now();

            if (ready <= 0) {
                // ★ 굶주림 워치독: 접속이 "없을 때"도 체크해야 재기동이 됨
                if (!draining && has_pending_work()) {
                    auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_).count();
                    if (idle >= settings.master_starvation_seconds) {
                        if (settings.run_local_worker_on_master &&
                            emergency_local_spawned_ < settings.emergency_local_spawn_max) {

                            const std::string selfExe = getSelfExePath();
                            int rc = startLocalWorkerNewWindow(selfExe, "127.0.0.1", this->port);
                            ++emergency_local_spawned_;
                            last_progress_ = now;
                            // (로그는 기존 코드 그대로)
                        }
                    }
                }
                if (draining && now >= drain_deadline) break;
                continue;
            }

            sockaddr_in client_addr{}; socklen_t client_len = sizeof(client_addr);
            SOCKET client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_sock == INVALID_SOCKET) continue;

            char ipStr[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, INET_ADDRSTRLEN);
            std::wcout << L"Worker connected: " << utf8_to_wstring(ipStr) << L"\n";

            int cid = INT32_MAX;
            bool has_work = (!draining) && try_acquire_next_chunk(cid);

            if (has_work) {
                // 청크 전송
                std::string chunk_data = chunks[cid].serialize();
                bool sent_ok = NetworkUtils::sendData(client_sock, chunk_data);
                if (!sent_ok) {
                    std::wcout << L"[WARN ] Failed to send chunk " << cid << L" to worker. Requeue.\n";
                    requeue_chunk_if_needed(cid);
                }
                else {
                    std::wcout << L"Sent chunk " << cid << L" to worker\n";
                    // 결과 수신
                    std::string result_data = NetworkUtils::receiveData(client_sock);
                    if (result_data.empty()) {
                        std::wcout << L"[WARN ] Worker disconnected before sending result for chunk "
                            << cid << L". Requeue.\n";
                        requeue_chunk_if_needed(cid);
                    }
                    else {
                        ProcessResult r = ProcessResult::deserialize(result_data);
                        // 중복 결과 방지(느리게 도착한 중복 결과/재배정 후 중복 대비)
                        bool first_time = false;
                        {
                            std::lock_guard<std::mutex> lk(results_mutex);
                            if (completed_chunk_ids_.insert(r.chunk_id).second) {
                                results.push_back(r);
                                first_time = true;
                                std::wcout << L"Received result for chunk " << r.chunk_id
                                    << L": avg_distance=" << r.avg_distance
                                    << L", points=" << r.point_count << L"\n";
                            }
                            else {
                                std::wcout << L"[INFO ] Duplicate result ignored for chunk "
                                    << r.chunk_id << L"\n";
                            }

                            // 모든 청크 완료 → 드레이닝 시작
                            if (completed_chunk_ids_.size() == chunks.size() && !draining) {
                                all_chunks_processed.store(true);
                                draining = true;
                                drain_deadline = std::chrono::steady_clock::now()
                                    + std::chrono::seconds(settings.drain_seconds);
                                std::wcout << L"All chunks processed! Entering draining phase for "
                                    << settings.drain_seconds << L"s...\n";
                            }
                        }
                    }

                    last_progress_ = std::chrono::steady_clock::now();
                }
            }
            else {
                /// 더 줄 일이 없으면 종료 신호
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

    /// 재배당 대기열 우선 → 없으면 전진 인덱스에서 하나 할당
    bool try_acquire_next_chunk(int& out_cid) {
        std::lock_guard<std::mutex> lk(retry_mtx_);
        if (!retry_queue_.empty()) {
            out_cid = retry_queue_.front();
            retry_queue_.pop_front();
            return true;
        }
        int idx = chunk_index.fetch_add(1);
        if (idx < static_cast<int>(chunks.size())) {
            out_cid = idx;
            return true;
        }
        return false;
    }

    /// 실패/중단 시 청크 재배치
    void requeue_chunk_if_needed(int cid) {
        std::lock_guard<std::mutex> lk(retry_mtx_);
        // 이미 결과를 받은 청크는 재배정하지 않음(중복 방지)
        if (completed_chunk_ids_.find(cid) == completed_chunk_ids_.end()) {
            retry_queue_.push_back(cid);
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
        if (chunk.points.size() > 0)
            r.avg_distance = total_distance / static_cast<float>(chunk.points.size());
        else
            r.avg_distance = 0.f;
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
