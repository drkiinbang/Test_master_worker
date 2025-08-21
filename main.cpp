///////////////////////////////////////////////////////////////////////////////
/// Point Cloud Distributed Processing System (Patched: password & key auth)
///
/// 본 파일은 사용자가 업로드한 main.cpp를 기반으로, 원격 워커 실행 시
/// 공개키(key) 외에 username/password 인증도 지원하도록 통합 패치한 버전입니다.
/// - Windows: password 모드일 때 PuTTY plink의 -pw 사용 (비대화식)
/// - Linux/macOS: password 모드일 때 sshpass -p ... ssh 사용
/// - 키 기반은 기존 ssh 그대로 유지, BatchMode=yes 적용
/// - workers.conf 파서 확장: 5번째 토큰부터 k=v 형태 해석(auth, password, ssh_client, hostkey, os)
///
/// 사용 예 (workers.conf):
///   10.10.10.17,kiinbang,C:\\Workers\\Test_master_worker.exe,22,windows,auth=key,ssh_client=ssh,hostkey=SHA256:YOUR_HOSTKEY
///   10.10.10.18,kiinbang,/home/kiinbang/Workers/Test_master_worker,22,linux,auth=password,password=MyP@ss,ssh_client=sshpass
///   10.10.10.19,kiinbang,C:\\Workers\\Test_master_worker.exe,22,windows,auth=password,password=MyP@ss,ssh_client=plink,hostkey=SHA256:YOUR_HOSTKEY
///////////////////////////////////////////////////////////////////////////////

/// Silence deprecation warnings for <codecvt> on MSVC
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING 1

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

/// Simple quote-escape helper for shell-safe command building (macOS/Linux/Windows)
static std::string escapeDQ(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) out += (c == '"') ? "\\\"" : std::string(1, c);
    return out;
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket close
#endif

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
static inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// 로컬 IP 자동 탐지(IPv4)
static std::string getLocalIPAddress() {
    std::string local_ip = "127.0.0.1";
#ifdef _WIN32
    char hostname[256] = { 0 };
    if (gethostname(hostname, sizeof(hostname)) != 0) return local_ip;
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || !result) return local_ip;
    char ipStr[INET_ADDRSTRLEN] = { 0 };
    auto* a = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    if (inet_ntop(AF_INET, &(a->sin_addr), ipStr, sizeof(ipStr))) local_ip = ipStr;
    freeaddrinfo(result);
#else
    struct ifaddrs* ifaddr = nullptr; if (getifaddrs(&ifaddr) == -1) return local_ip;
    for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa || !ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET && !(ifa->ifa_flags & IFF_LOOPBACK)) {
            char ip[INET_ADDRSTRLEN] = { 0 };
            void* addr_ptr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
            if (inet_ntop(AF_INET, addr_ptr, ip, sizeof(ip))) { local_ip = ip; break; }
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
    int ssh_port;                         // default 22

    // Extended auth controls
    std::string auth_method = "key";     // "key" or "password"
    std::string password;                 // used if auth=password
    std::string ssh_client;               // "ssh"|"plink"|"sshpass" (auto if empty)
    std::string hostkey;                  // e.g., "SHA256:xxxx" to pin host key
    std::string os_hint;                  // optional: "windows"|"linux"|"mac"

    RemoteWorkerConfig() : ssh_port(22) {}
    RemoteWorkerConfig(const std::string& ip, const std::string& user,
        const std::string& path, int port = 22)
        : ip_address(ip), username(user), worker_path(path), ssh_port(port) {
    }
};

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#include <vector>
#include <string>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static std::string getSelfExePath() {
#ifdef _WIN32
    std::wstring wpath; DWORD cap = 260;
    for (;;) {
        std::vector<wchar_t> buf(cap);
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::string();
        if (n < buf.size() - 1) { wpath.assign(buf.data(), n); break; }
        cap *= 2;
    }
    return wstring_to_utf8(wpath);
#elif __APPLE__
    uint32_t size = 0; _NSGetExecutablePath(nullptr, &size); std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::string();
    char resolved[PATH_MAX] = { 0 };
    if (realpath(buf.data(), resolved)) return std::string(resolved);
    return std::string(buf.data());
#else
    std::vector<char> buf(4096); ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return std::string(); buf[n] = '\0';
    char resolved[PATH_MAX] = { 0 }; if (realpath(buf.data(), resolved)) return std::string(resolved);
    return std::string(buf.data());
#endif
}

static int startLocalWorkerNewWindow(const std::string& exePath,
    const std::string& master_ip,
    int master_port) {
    const std::string base = "\"" + exePath + "\" worker " + master_ip + " " + std::to_string(master_port);
#if defined(_WIN32)
    std::string cmd = "cmd /c start \"\" " + base; return std::system(cmd.c_str());
#elif defined(__APPLE__)
    std::string bashLine = "bash -lc \\\"" + escapeDQ(base) + "; exec bash -i\\\"";
    std::string cmd = "osascript -e 'tell application \"Terminal\" to do script \"" + bashLine + "\"' -e 'tell application \"Terminal\" to activate'";
    return std::system(cmd.c_str());
#else
    std::string inner =
        "if [ -z \"$DISPLAY\" ]; then export DISPLAY=:0; fi; "
        "if command -v gnome-terminal >/dev/null 2>&1; then "
        "gnome-terminal -- bash -lc \"" + escapeDQ(base) + "; exec bash -i\"; "
        "elif command -v konsole >/dev/null 2>&1; then "
        "konsole --hold -e bash -lc \"" + escapeDQ(base) + "; exec bash -i\"; "
        "elif command -v xterm >/dev/null 2>&1; then "
        "xterm -hold -e \"" + escapeDQ(base) + "\"; "
        "else tmux new-session -d -s pointcloud \"" + escapeDQ(base) + "\"; echo 'No desktop terminal; started in tmux session' ; fi";
    std::string cmd = "bash -lc \"" + escapeDQ(inner) + "\""; return std::system(cmd.c_str());
#endif
}

struct RuntimeSettings {
    int drain_seconds = 5;
    int worker_idle_timeout_seconds = 20;
    int worker_retry_max = 5;
    int worker_retry_backoff_ms = 200;
    int worker_recv_timeout_ms = 5000;
    int worker_send_timeout_ms = 5000;
    int  ssh_connect_timeout_sec = 5;
    int  ssh_server_alive_interval_sec = 5;
    int  ssh_server_alive_count_max = 2;
    bool ssh_batch_mode = true;
    std::string ssh_strict_host_key = "accept-new";
    bool run_local_worker_on_master = false;
    int master_starvation_seconds = 30;
    int emergency_local_spawn_max = 3;
};

class ConfigManager {
public:
    static void createDefaultConfig(const std::string& config_file) {
        std::ofstream file(config_file);
        if (!file.is_open()) return;

        file << "# Remote Worker Configuration File\n";
        file << "# Lines with commas are remote workers: ip,username,worker_path[,ssh_port][,os][,k=v ...]\\n";
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

        file << "# --- options ---\n";
        file << "run_local_worker_on_master=true\n\n";

        file << "# --- workers (examples) ---\n";
        file << "# ip,username,worker_path[,ssh_port][,os][,k=v ...]\n";
        file << "# keys: auth=key|password, password=..., ssh_client=ssh|plink|sshpass, hostkey=SHA256:..., os=windows|linux|mac\n";
        file << "#10.10.10.17,kiinbang,C:\\Workers\\Test_master_worker.exe,22,windows,auth=key,ssh_client=ssh,hostkey=SHA256:YOUR_HOSTKEY\n";
        file << "#10.10.10.18,kiinbang,/Users/kiinbang/Workers/Test_master_worker,22,mac,auth=password,password=MyP@ss,ssh_client=sshpass\n";
        file << "#10.10.10.19,kiinbang,/home/kiinbang/Workers/Test_master_worker,22,linux,auth=password,password=MyP@ss,ssh_client=sshpass\n";
        file << "#10.10.10.21,kiinbang,C:\\Workers\\Test_master_worker.exe,22,windows,auth=password,password=MyP@ss,ssh_client=plink,hostkey=SHA256:YOUR_HOSTKEY\n";
    }

    static void ensureConfig(const std::string& config_file) {
        std::ifstream f(config_file); if (!f.is_open()) createDefaultConfig(config_file);
    }

    static std::vector<RemoteWorkerConfig> loadRemoteWorkers(const std::string& config_file) {
        std::vector<RemoteWorkerConfig> workers;
        std::ifstream file(config_file);
        if (!file.is_open()) {
            std::wcout << L"Config file not found: " << utf8_to_wstring(config_file) << L". Running with local workers only.\n";
            return workers;
        }
        std::string line;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.find(',') != std::string::npos) {
                auto tokens = split(line, ',');
                if (tokens.size() >= 3) {
                    RemoteWorkerConfig cfg;
                    cfg.ip_address = trim(tokens[0]);
                    cfg.username = trim(tokens[1]);
                    cfg.worker_path = trim(tokens[2]);
                    if (tokens.size() >= 4 && !trim(tokens[3]).empty()) { try { cfg.ssh_port = std::stoi(trim(tokens[3])); } catch (...) {} }
                    // 5th+ tokens: k=v OR legacy OS token
                    for (size_t i = 4; i < tokens.size(); ++i) {
                        auto t = trim(tokens[i]); if (t.empty()) continue;
                        auto kv = split(t, '=');
                        if (kv.size() != 2) { if (i == 4) cfg.os_hint = to_lower(t); continue; }
                        auto k = to_lower(trim(kv[0])); auto v = trim(kv[1]);
                        if (k == "auth") cfg.auth_method = to_lower(v);
                        else if (k == "password") cfg.password = v;
                        else if (k == "ssh_client") cfg.ssh_client = to_lower(v);
                        else if (k == "hostkey") cfg.hostkey = v;
                        else if (k == "os") cfg.os_hint = to_lower(v);
                    }
                    workers.push_back(cfg);
                    std::wcout << L"Loaded remote worker: " << utf8_to_wstring(cfg.ip_address)
                        << L" (" << utf8_to_wstring(cfg.username) << L")\n";
                }
            }
        }
        return workers;
    }

    static RuntimeSettings loadRuntimeSettings(const std::string& config_file) {
        RuntimeSettings s; std::ifstream file(config_file); if (!file.is_open()) return s;
        std::string line;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.find('=') == std::string::npos || line.find(',') != std::string::npos) continue;
            auto pos = line.find('='); std::string k = trim(line.substr(0, pos)); std::string v = trim(line.substr(pos + 1));
            auto lcase = [](std::string t) { std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); }); return t; };
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
                else if (lk == "run_local_worker_on_master") s.run_local_worker_on_master = (lv == "1" || lv == "true");
                else if (lk == "master_starvation_seconds") s.master_starvation_seconds = std::stoi(v);
                else if (lk == "emergency_local_spawn_max") s.emergency_local_spawn_max = std::stoi(v);
            }
            catch (...) { std::wcout << L"[Config] Failed to parse key: " << utf8_to_wstring(k) << L"\n"; }
        }
        return s;
    }
};

class RemoteWorkerManager {
private:
    std::vector<RemoteWorkerConfig> remote_configs;
    std::vector<std::thread> remote_threads;
    std::string master_ip; int master_port; RuntimeSettings settings;
    std::atomic<bool> should_stop{ false };

public:
    RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
        const std::string& master_ip, int master_port,
        const RuntimeSettings& settings)
        : remote_configs(configs), master_ip(master_ip), master_port(master_port), settings(settings) {
    }
    ~RemoteWorkerManager() { stop(); }

    void startRemoteWorkers() {
        for (const auto& cfg : remote_configs) remote_threads.emplace_back([this, cfg]() { this->runRemoteWorker(cfg); });
        std::wcout << L"Started " << remote_configs.size() << L" remote workers\n";
    }
    void stop() { should_stop.store(true); for (auto& t : remote_threads) if (t.joinable()) t.join(); remote_threads.clear(); }

private:
    void runRemoteWorker(const RemoteWorkerConfig& config) {
        std::wcout << L"Starting remote worker on " << utf8_to_wstring(config.ip_address) << L"\n";
        std::stringstream ssh_command;

        const bool use_password = (to_lower(config.auth_method) == "password");
#ifdef _WIN32
        const std::string client = !config.ssh_client.empty() ? config.ssh_client : (use_password ? "plink" : "ssh");
        if (client == "plink") {
            ssh_command << "plink -ssh -P " << config.ssh_port
                << " -l " << config.username;
            if (use_password && !config.password.empty())
                ssh_command << " -pw \"" << escapeDQ(config.password) << "\"";
            ssh_command << " -batch";
            if (!config.hostkey.empty())
                ssh_command << " -hostkey \"" << escapeDQ(config.hostkey) << "\"";
            ssh_command << " " << config.ip_address
                << " \"" << escapeDQ(config.worker_path)
                << " worker " << master_ip << " " << master_port << " workers.conf\"";
        }
        else {
            ssh_command << "ssh -p " << config.ssh_port
                << " -o ConnectTimeout=" << settings.ssh_connect_timeout_sec
                << " -o ServerAliveInterval=" << settings.ssh_server_alive_interval_sec
                << " -o ServerAliveCountMax=" << settings.ssh_server_alive_count_max
                << " -o StrictHostKeyChecking=" << settings.ssh_strict_host_key;
            if (settings.ssh_batch_mode && !use_password) ssh_command << " -o BatchMode=yes";
            ssh_command << " " << config.username << "@" << config.ip_address
                << " \"" << escapeDQ(config.worker_path)
                << " worker " << master_ip << " " << master_port << " workers.conf\""
                << " < NUL";
        }
#else
        const std::string client = !config.ssh_client.empty() ? config.ssh_client : (use_password ? "sshpass" : "ssh");
        if (client == "sshpass" && use_password) {
            ssh_command << "sshpass -p \"" << escapeDQ(config.password) << "\" "
                << "ssh -p " << config.ssh_port
                << " -o StrictHostKeyChecking=" << settings.ssh_strict_host_key
                << " -o ConnectTimeout=" << settings.ssh_connect_timeout_sec
                << " -o ServerAliveInterval=" << settings.ssh_server_alive_interval_sec
                << " -o ServerAliveCountMax=" << settings.ssh_server_alive_count_max
                << " -n "
                << config.username << "@" << config.ip_address
                << " '" << config.worker_path
                << " worker " << master_ip << " " << master_port << " workers.conf'";
        }
        else {
            ssh_command << "ssh -p " << config.ssh_port
                << " -o StrictHostKeyChecking=" << settings.ssh_strict_host_key
                << " -o ConnectTimeout=" << settings.ssh_connect_timeout_sec
                << " -o ServerAliveInterval=" << settings.ssh_server_alive_interval_sec
                << " -o ServerAliveCountMax=" << settings.ssh_server_alive_count_max
                << " -n";
            if (settings.ssh_batch_mode) ssh_command << " -o BatchMode=yes";
            ssh_command << " " << config.username << "@" << config.ip_address
                << " '" << config.worker_path
                << " worker " << master_ip << " " << master_port << " workers.conf'";
        }
#endif
        const std::string cmd = ssh_command.str();
        std::wcout << L"Executing: " << utf8_to_wstring(cmd) << L"\n";

        while (!should_stop.load()) {
            int rc = std::system(cmd.c_str());
            if (rc == 0) std::wcout << L"Remote worker on " << utf8_to_wstring(config.ip_address) << L" completed successfully\n";
            else          std::wcout << L"Remote worker on " << utf8_to_wstring(config.ip_address) << L" failed or disconnected (code: " << rc << L")\n";
            if (!should_stop.load()) std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        std::wcout << L"Remote worker thread for " << utf8_to_wstring(config.ip_address) << L" terminated\n";
    }
};

// ===================== 아래는 원본의 Master/Worker 구현을 유지 =====================
struct Point3D { float x, y, z; Point3D(float x = 0, float y = 0, float z = 0) :x(x), y(y), z(z) {} };
struct PointCloudChunk {
    int chunk_id{}; std::vector<Point3D> points;
    std::string serialize() const { std::stringstream ss; ss << chunk_id << " " << points.size() << " "; for (auto& p : points) ss << p.x << " " << p.y << " " << p.z << " "; return ss.str(); }
    static PointCloudChunk deserialize(const std::string& data) { PointCloudChunk c; std::stringstream ss(data); size_t n = 0; ss >> c.chunk_id >> n; c.points.reserve(n); for (size_t i = 0; i < n; ++i) { Point3D p; ss >> p.x >> p.y >> p.z; c.points.push_back(p); } return c; }
};
struct ProcessResult {
    int chunk_id{}; float avg_distance{}; int point_count{};
    std::string serialize() const { std::stringstream ss; ss << chunk_id << " " << avg_distance << " " << point_count; return ss.str(); }
    static ProcessResult deserialize(const std::string& s) { ProcessResult r; std::stringstream ss(s); ss >> r.chunk_id >> r.avg_distance >> r.point_count; return r; }
};
class NetworkUtils {
public:
    static bool sendData(SOCKET s, const std::string& data) {
        uint32_t size = static_cast<uint32_t>(data.size());
        if (send(s, reinterpret_cast<const char*>(&size), sizeof(size), 0) != sizeof(size)) return false;
        const char* buf = data.data(); int total = 0, need = static_cast<int>(size);
        while (total < need) { int sent = send(s, buf + total, need - total, 0); if (sent == SOCKET_ERROR) return false; total += sent; }
        return true;
    }
    static std::string receiveData(SOCKET s) {
        uint32_t size = 0; int recvd = recv(s, reinterpret_cast<char*>(&size), sizeof(size), 0); if (recvd != sizeof(size)) return "";
        std::string data(size, '\0'); int total = 0, need = static_cast<int>(size);
        while (total < need) { int got = recv(s, &data[total], need - total, 0); if (got == SOCKET_ERROR || got == 0) return ""; total += got; }
        return data;
    }
    static bool sendTerminationSignal(SOCKET s) { static const std::string k = "TERMINATE"; return sendData(s, k); }
    static bool isTerminationSignal(const std::string& d) { return d == "TERMINATE"; }
};

class MasterServer {
private:
    std::vector<PointCloudChunk> chunks; std::vector<ProcessResult> results; std::atomic<int> chunk_index{ 0 };
    std::mutex results_mutex; int port; std::atomic<bool> all_chunks_processed{ false };
    std::unique_ptr<RemoteWorkerManager> remote_manager; std::string config_file; RuntimeSettings settings;
    std::vector<RemoteWorkerConfig> remote_configs; std::deque<int> retry_queue_; std::mutex retry_mtx_;
    std::unordered_set<int> completed_chunk_ids_; std::chrono::steady_clock::time_point last_progress_{ std::chrono::steady_clock::now() };
    int emergency_local_spawned_ = 0;
public:
    MasterServer(int p, const std::string& config = "workers.conf") : port(p), config_file(config) {}
    ~MasterServer() { if (remote_manager) remote_manager->stop(); }

    void generateSampleData() {
        std::random_device rd; std::mt19937 gen(rd()); std::uniform_real_distribution<float> dis(-100.f, 100.f);
        for (int cid = 0; cid < 100; ++cid) { PointCloudChunk c; c.chunk_id = cid; for (int i = 0; i < 1000; ++i) c.points.emplace_back(dis(gen), dis(gen), dis(gen)); chunks.push_back(std::move(c)); }
        std::wcout << L"Generated " << chunks.size() << L" chunks with total " << chunks.size() * 1000 << L" points\n";
    }

    bool has_pending_work() const { return completed_chunk_ids_.size() < chunks.size(); }

    void start() {
        ConfigManager::ensureConfig(config_file); settings = ConfigManager::loadRuntimeSettings(config_file); remote_configs = ConfigManager::loadRemoteWorkers(config_file);
        if (settings.run_local_worker_on_master) {
            const std::string selfExe = getSelfExePath(); int rc = startLocalWorkerNewWindow(selfExe, "127.0.0.1", this->port);
#ifdef _WIN32
            if (rc != 0) std::wcerr << L"[WARN ] Local worker start failed (rc=" << rc << L")\n"; else std::wcout << L"[ OK  ] Local worker started in a new window\n";
#else
            if (rc != 0) std::cerr << "[WARN ] Local worker start failed (rc=" << rc << ")\n"; else std::cout << "[ OK  ] Local worker started in a new window\n";
#endif
        }
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0); if (server_sock == INVALID_SOCKET) { std::wcerr << L"Socket creation failed\n"; return; }
        int opt = 1; setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons(port);
        if (bind(server_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) { std::wcerr << L"Bind failed\n"; closesocket(server_sock); return; }
        if (listen(server_sock, 10) == SOCKET_ERROR) { std::wcerr << L"Listen failed\n"; closesocket(server_sock); return; }
        std::wcout << L"Master server listening on port " << port << L"\n";
        if (!remote_configs.empty()) {
            std::string master_ip = getLocalIPAddress(); remote_manager = std::make_unique<RemoteWorkerManager>(remote_configs, master_ip, port, settings);
            std::wcout << L"Starting remote workers (Master IP: " << utf8_to_wstring(master_ip) << L":" << port << L")...\n"; remote_manager->startRemoteWorkers(); std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        else { std::wcout << L"No remote workers configured. Running with local workers only.\n"; }
        bool draining = false; auto drain_deadline = (std::chrono::steady_clock::time_point::max)();
        while (true) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(server_sock, &rfds); timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
#ifdef _WIN32
            int ready = select(0, &rfds, nullptr, nullptr, &tv);
#else
            int ready = select(server_sock + 1, &rfds, nullptr, nullptr, &tv);
#endif
            auto now = std::chrono::steady_clock::now();
            if (ready <= 0) {
                if (!draining && has_pending_work()) { auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_).count(); if (idle >= settings.master_starvation_seconds) { if (settings.run_local_worker_on_master && emergency_local_spawned_ < settings.emergency_local_spawn_max) { const std::string selfExe = getSelfExePath(); int rc = startLocalWorkerNewWindow(selfExe, "127.0.0.1", this->port); ++emergency_local_spawned_; last_progress_ = now; } } }
                if (draining && now >= drain_deadline) break; continue;
            }
            sockaddr_in client_addr{}; socklen_t client_len = sizeof(client_addr); SOCKET client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len); if (client_sock == INVALID_SOCKET) continue;
            char ipStr[INET_ADDRSTRLEN] = { 0 }; inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, INET_ADDRSTRLEN); std::wcout << L"Worker connected: " << utf8_to_wstring(ipStr) << L"\n";
            int cid = INT32_MAX; bool has_work = (!draining) && try_acquire_next_chunk(cid);
            if (has_work) {
                std::string chunk_data = chunks[cid].serialize(); bool sent_ok = NetworkUtils::sendData(client_sock, chunk_data);
                if (!sent_ok) { std::wcout << L"[WARN ] Failed to send chunk " << cid << L" to worker. Requeue.\n"; requeue_chunk_if_needed(cid); }
                else {
                    std::wcout << L"Sent chunk " << cid << L" to worker\n"; std::string result_data = NetworkUtils::receiveData(client_sock);
                    if (result_data.empty()) { std::wcout << L"[WARN ] Worker disconnected before sending result for chunk " << cid << L". Requeue.\n"; requeue_chunk_if_needed(cid); }
                    else {
                        ProcessResult r = ProcessResult::deserialize(result_data); bool first_time = false; {
                            std::lock_guard<std::mutex> lk(results_mutex);
                            if (completed_chunk_ids_.insert(r.chunk_id).second) { results.push_back(r); first_time = true; std::wcout << L"Received result for chunk " << r.chunk_id << L": avg_distance=" << r.avg_distance << L", points=" << r.point_count << L"\n"; }
                            else { std::wcout << L"[INFO ] Duplicate result ignored for chunk " << r.chunk_id << L"\n"; }
                            if (completed_chunk_ids_.size() == chunks.size() && !draining) { all_chunks_processed.store(true); draining = true; drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(settings.drain_seconds); std::wcout << L"All chunks processed! Entering draining phase for " << settings.drain_seconds << L"s...\n"; }
                        }
                    }
                    last_progress_ = std::chrono::steady_clock::now();
                }
            }
            else { NetworkUtils::sendTerminationSignal(client_sock); std::wcout << L"Sent termination signal to worker\n"; }
            closesocket(client_sock);
        }
        closesocket(server_sock); printFinalResults(); if (remote_manager) { std::wcout << L"Shutting down remote workers...\n"; remote_manager->stop(); }
    }
private:
    void printFinalResults() { std::wcout << L"\n=== Final Results ===\n"; float total_avg = 0.0f; int total_points = 0; for (const auto& r : results) { total_avg += r.avg_distance; total_points += r.point_count; std::wcout << L"Chunk " << r.chunk_id << L": " << r.point_count << L" points, avg_distance=" << r.avg_distance << L"\n"; } if (!results.empty()) { std::wcout << L"Overall average distance: " << (total_avg / results.size()) << L"\n"; std::wcout << L"Total points processed: " << total_points << L"\n"; } }
    bool try_acquire_next_chunk(int& out_cid) { std::lock_guard<std::mutex> lk(retry_mtx_); if (!retry_queue_.empty()) { out_cid = retry_queue_.front(); retry_queue_.pop_front(); return true; } int idx = chunk_index.fetch_add(1); if (idx < static_cast<int>(chunks.size())) { out_cid = idx; return true; } return false; }
    void requeue_chunk_if_needed(int cid) { std::lock_guard<std::mutex> lk(retry_mtx_); if (completed_chunk_ids_.find(cid) == completed_chunk_ids_.end()) retry_queue_.push_back(cid); }
};

class WorkerClient {
private:
    std::string master_ip; int master_port; RuntimeSettings settings; bool last_was_terminate_ = false;
public:
    WorkerClient(const std::string& ip, int port, const RuntimeSettings& s) : master_ip(ip), master_port(port), settings(s) {}
    void start() { using clock = std::chrono::steady_clock; auto last_success = clock::now(); int fail_count = 0; std::wcout << L"Worker started. Connecting to master...\n"; while (true) { if (!connectAndProcess()) { if (last_was_terminate_) { std::wcout << L"Worker terminated by master.\n"; break; } auto idle_elapsed = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - last_success).count(); if (idle_elapsed >= settings.worker_idle_timeout_seconds || ++fail_count >= settings.worker_retry_max) { std::wcout << L"No more work or no response from master. Exiting.\n"; break; } int backoff = settings.worker_retry_backoff_ms * fail_count; std::this_thread::sleep_for(std::chrono::milliseconds(backoff)); continue; } fail_count = 0; last_success = clock::now(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); } }
private:
    bool connectAndProcess() {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0); if (sock == INVALID_SOCKET) { std::wcerr << L"Socket creation failed\n"; return false; }
#ifdef _WIN32
        DWORD rcv = settings.worker_recv_timeout_ms; DWORD snd = settings.worker_send_timeout_ms; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof(rcv)); setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof(snd));
#else
        auto to_tv = [](int ms) { timeval tv{}; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000; return tv; }; timeval rtv = to_tv(settings.worker_recv_timeout_ms); timeval stv = to_tv(settings.worker_send_timeout_ms); setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv)); setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
#endif
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(master_port); if (inet_pton(AF_INET, master_ip.c_str(), &addr.sin_addr) != 1) { std::wcerr << L"Invalid IP address\n"; closesocket(sock); return false; }
        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) { std::wcerr << L"Connection to master failed\n"; closesocket(sock); return false; }
        std::string received_data = NetworkUtils::receiveData(sock); if (received_data.empty()) { std::wcerr << L"Failed to receive data from master\n"; closesocket(sock); last_was_terminate_ = false; return false; }
        if (NetworkUtils::isTerminationSignal(received_data)) { std::wcout << L"Received termination signal from master\n"; closesocket(sock); last_was_terminate_ = true; return false; }
        PointCloudChunk chunk = PointCloudChunk::deserialize(received_data); std::wcout << L"Received chunk " << chunk.chunk_id << L" with " << chunk.points.size() << L" points\n";
        ProcessResult result = processPointCloud(chunk); NetworkUtils::sendData(sock, result.serialize()); std::wcout << L"Sent processing result for chunk " << chunk.chunk_id << L"\n";
        closesocket(sock); last_was_terminate_ = false; return true;
    }
    ProcessResult processPointCloud(const PointCloudChunk& chunk) { std::wcout << L"Processing chunk " << chunk.chunk_id << L"...\n"; float total_distance = 0.0f; for (const auto& p : chunk.points) { float d = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z); total_distance += d; } std::this_thread::sleep_for(std::chrono::milliseconds(500)); ProcessResult r; r.chunk_id = chunk.chunk_id; r.avg_distance = chunk.points.empty() ? 0.f : total_distance / static_cast<float>(chunk.points.size()); r.point_count = static_cast<int>(chunk.points.size()); std::wcout << L"Finished processing chunk " << chunk.chunk_id << L" (avg_distance: " << r.avg_distance << L")\n"; return r; }
};

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_U16TEXT); WSADATA wsaData; if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) { std::wcerr << L"WSAStartup failed\n"; return 1; }
    if (argc < 2) { std::wcout << L"Usage:\n" << L"  Master mode: " << argv[0] << L" master [port] [config_file]\n" << L"  Worker mode: " << argv[0] << L" worker [master_ip] [master_port] [config_file]\n"; WSACleanup(); return 1; }
    std::wstring mode = argv[1]; if (mode == L"master") { int port = (argc >= 3) ? _wtoi(argv[2]) : 8080; std::string cfg = (argc >= 4) ? wstring_to_utf8(argv[3]) : "workers.conf"; MasterServer server(port, cfg); server.generateSampleData(); server.start(); }
    else if (mode == L"worker") { std::wstring master_ip_w = (argc >= 3) ? argv[2] : L"127.0.0.1"; int master_port = (argc >= 4) ? _wtoi(argv[3]) : 8080; std::string cfg = (argc >= 5) ? wstring_to_utf8(argv[4]) : "workers.conf"; ConfigManager::ensureConfig(cfg); RuntimeSettings s = ConfigManager::loadRuntimeSettings(cfg); WorkerClient worker(wstring_to_utf8(master_ip_w), master_port, s); worker.start(); }
    else { std::wcerr << L"Invalid mode. Use 'master' or 'worker'\n"; WSACleanup(); return 1; }
    WSACleanup(); return 0;
}
#else
int main(int argc, char* argv[]) {
    if (argc < 2) { std::cout << "Usage:\n" << "  Master mode: " << argv[0] << " master [port] [config_file]\n" << "  Worker mode: " << argv[0] << " worker [master_ip] [master_port] [config_file]\n"; return 1; }
    std::string mode = argv[1]; if (mode == "master") { int port = (argc >= 3) ? std::stoi(argv[2]) : 8080; std::string cfg = (argc >= 4) ? argv[3] : "workers.conf"; MasterServer server(port, cfg); server.generateSampleData(); server.start(); }
    else if (mode == "worker") { std::string ip = (argc >= 3) ? argv[2] : "127.0.0.1"; int port = (argc >= 4) ? std::stoi(argv[3]) : 8080; std::string cfg = (argc >= 5) ? argv[4] : "workers.conf"; ConfigManager::ensureConfig(cfg); RuntimeSettings s = ConfigManager::loadRuntimeSettings(cfg); WorkerClient worker(ip, port, s); worker.start(); }
    else { std::cerr << "Invalid mode. Use 'master' or 'worker'\n"; return 1; } return 0;
}
#endif
