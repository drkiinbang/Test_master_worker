///////////////////////////////////////////////////////////////////////////////
/// Point Cloud Distributed Processing System (Refactored for Readability)
///
/// This system supports both key-based and password-based SSH authentication
/// for remote worker execution with improved code organization and readability.
///////////////////////////////////////////////////////////////////////////////

#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING 1

// Standard library includes
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

// Platform-specific includes
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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
#include <limits.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket close
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

///////////////////////////////////////////////////////////////////////////////
// Utility Functions
///////////////////////////////////////////////////////////////////////////////

namespace Utils {
    // String utilities
    std::string escapeDQ(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            result += (c == '"') ? "\\\"" : std::string(1, c);
        }
        return result;
    }

    std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> tokens;
        std::stringstream ss(s);
        std::string token;

        while (std::getline(ss, token, delim)) {
            tokens.push_back(token);
        }
        return tokens;
    }

    std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";

        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string toLowerCase(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    // UTF-8/Wide string conversion utilities
    std::string wstringToUtf8(const std::wstring& wstr) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.to_bytes(wstr);
    }

    std::wstring utf8ToWstring(const std::string& str) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.from_bytes(str);
    }

    // Boolean parsing utility
    bool parseBool(const std::string& value) {
        std::string lowerValue = toLowerCase(value);
        return (lowerValue == "1" || lowerValue == "true" ||
            lowerValue == "yes" || lowerValue == "on");
    }
}

///////////////////////////////////////////////////////////////////////////////
// Network Utilities
///////////////////////////////////////////////////////////////////////////////

namespace Network {
    std::string getLocalIPAddress() {
        std::string localIP = "127.0.0.1";

#ifdef _WIN32
        char hostname[256] = { 0 };
        if (gethostname(hostname, sizeof(hostname)) != 0) {
            return localIP;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* result = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || !result) {
            return localIP;
        }

        char ipStr[INET_ADDRSTRLEN] = { 0 };
        auto* addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
        if (inet_ntop(AF_INET, &(addr->sin_addr), ipStr, sizeof(ipStr))) {
            localIP = ipStr;
        }
        freeaddrinfo(result);

#else
        struct ifaddrs* ifaddr = nullptr;
        if (getifaddrs(&ifaddr) == -1) {
            return localIP;
        }

        for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa || !ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET &&
                !(ifa->ifa_flags & IFF_LOOPBACK)) {
                char ip[INET_ADDRSTRLEN] = { 0 };
                void* addrPtr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                if (inet_ntop(AF_INET, addrPtr, ip, sizeof(ip))) {
                    localIP = ip;
                    break;
                }
            }
        }
        freeifaddrs(ifaddr);
#endif
        return localIP;
    }

    class NetworkUtils {
    public:
        static bool sendData(SOCKET socket, const std::string& data) {
            uint32_t size = static_cast<uint32_t>(data.size());

            // Send size first
            if (send(socket, reinterpret_cast<const char*>(&size), sizeof(size), 0) != sizeof(size)) {
                return false;
            }

            // Send data
            const char* buffer = data.data();
            int totalSent = 0;
            int remaining = static_cast<int>(size);

            while (totalSent < remaining) {
                int sent = send(socket, buffer + totalSent, remaining - totalSent, 0);
                if (sent == SOCKET_ERROR) {
                    return false;
                }
                totalSent += sent;
            }
            return true;
        }

        static std::string receiveData(SOCKET socket) {
            uint32_t size = 0;
            int received = recv(socket, reinterpret_cast<char*>(&size), sizeof(size), 0);
            if (received != sizeof(size)) {
                return "";
            }

            std::string data(size, '\0');
            int totalReceived = 0;
            int remaining = static_cast<int>(size);

            while (totalReceived < remaining) {
                int received = recv(socket, &data[totalReceived], remaining - totalReceived, 0);
                if (received == SOCKET_ERROR || received == 0) {
                    return "";
                }
                totalReceived += received;
            }
            return data;
        }

        static bool sendTerminationSignal(SOCKET socket) {
            static const std::string terminationSignal = "TERMINATE";
            return sendData(socket, terminationSignal);
        }

        static bool isTerminationSignal(const std::string& data) {
            return data == "TERMINATE";
        }
    };
}

///////////////////////////////////////////////////////////////////////////////
// System Utilities
///////////////////////////////////////////////////////////////////////////////

namespace System {
    std::string getSelfExecutablePath() {
#ifdef _WIN32
        std::wstring widePath;
        DWORD capacity = 260;

        for (;;) {
            std::vector<wchar_t> buffer(capacity);
            DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (length == 0) return std::string();

            if (length < buffer.size() - 1) {
                widePath.assign(buffer.data(), length);
                break;
            }
            capacity *= 2;
        }
        return Utils::wstringToUtf8(widePath);

#elif __APPLE__
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(size);

        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return std::string();
        }

        char resolved[PATH_MAX] = { 0 };
        if (realpath(buffer.data(), resolved)) {
            return std::string(resolved);
        }
        return std::string(buffer.data());

#else
        std::vector<char> buffer(4096);
        ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length <= 0) return std::string();

        buffer[length] = '\0';
        char resolved[PATH_MAX] = { 0 };
        if (realpath(buffer.data(), resolved)) {
            return std::string(resolved);
        }
        return std::string(buffer.data());
#endif
    }

    int startLocalWorkerNewWindow(const std::string& exePath,
        const std::string& masterIP,
        int masterPort) {
        const std::string baseCommand = "\"" + exePath + "\" worker " +
            masterIP + " " + std::to_string(masterPort);

#if defined(_WIN32)
        std::string command = "cmd /c start \"\" " + baseCommand;
        return std::system(command.c_str());

#elif defined(__APPLE__)
        std::string bashLine = "bash -lc \\\"" + Utils::escapeDQ(baseCommand) +
            "; exec bash -i\\\"";
        std::string command = "osascript -e 'tell application \"Terminal\" to do script \"" +
            bashLine + "\"' -e 'tell application \"Terminal\" to activate'";
        return std::system(command.c_str());

#else
        std::string innerCommand =
            "if [ -z \"$DISPLAY\" ]; then export DISPLAY=:0; fi; "
            "if command -v gnome-terminal >/dev/null 2>&1; then "
            "gnome-terminal -- bash -lc \"" + Utils::escapeDQ(baseCommand) + "; exec bash -i\"; "
            "elif command -v konsole >/dev/null 2>&1; then "
            "konsole --hold -e bash -lc \"" + Utils::escapeDQ(baseCommand) + "; exec bash -i\"; "
            "elif command -v xterm >/dev/null 2>&1; then "
            "xterm -hold -e \"" + Utils::escapeDQ(baseCommand) + "\"; "
            "else "
            "tmux new-session -d -s pointcloud \"" + Utils::escapeDQ(baseCommand) + "\"; "
            "echo 'No desktop terminal; started in tmux session'; "
            "fi";

        std::string command = "bash -lc \"" + Utils::escapeDQ(innerCommand) + "\"";
        return std::system(command.c_str());
#endif
    }
}

///////////////////////////////////////////////////////////////////////////////
// Configuration Structures
///////////////////////////////////////////////////////////////////////////////

struct RemoteWorkerConfig {
    std::string ipAddress;
    std::string username;
    std::string workerPath;
    int sshPort = 22;

    // Extended authentication controls
    std::string authMethod = "key";  // "key" or "password"
    std::string password;
    std::string sshClient;           // "ssh", "plink", "sshpass" (auto if empty)
    std::string hostkey;             // e.g., "SHA256:xxxx" to pin host key
    std::string osHint;              // "windows", "linux", "mac"

    RemoteWorkerConfig() = default;

    RemoteWorkerConfig(const std::string& ip, const std::string& user,
        const std::string& path, int port = 22)
        : ipAddress(ip), username(user), workerPath(path), sshPort(port) {
    }
};

struct RuntimeSettings {
    int drainSeconds = 5;
    int workerIdleTimeoutSeconds = 20;
    int workerRetryMax = 5;
    int workerRetryBackoffMs = 200;
    int workerRecvTimeoutMs = 5000;
    int workerSendTimeoutMs = 5000;
    int sshConnectTimeoutSec = 5;
    int sshServerAliveIntervalSec = 5;
    int sshServerAliveCountMax = 2;
    bool sshBatchMode = true;
    std::string sshStrictHostKey = "accept-new";
    bool runLocalWorkerOnMaster = false;
    int masterStarvationSeconds = 30;
    int emergencyLocalSpawnMax = 3;
};

///////////////////////////////////////////////////////////////////////////////
// Configuration Management
///////////////////////////////////////////////////////////////////////////////

class ConfigManager {
public:
    static void createDefaultConfig(const std::string& configFile) {
        std::ofstream file(configFile);
        if (!file.is_open()) return;

        file << "# Remote Worker Configuration File\n"
            << "# Lines with commas are remote workers: ip,username,worker_path[,ssh_port][,os][,k=v ...]\n"
            << "# Lines with key=value are settings.\n\n"

            << "# ===== Settings (defaults) =====\n"
            << "drain_seconds=5\n"
            << "worker_idle_timeout_seconds=20\n"
            << "worker_retry_max=5\n"
            << "worker_retry_backoff_ms=200\n"
            << "worker_recv_timeout_ms=5000\n"
            << "worker_send_timeout_ms=5000\n"
            << "ssh_connect_timeout_sec=5\n"
            << "ssh_server_alive_interval_sec=5\n"
            << "ssh_server_alive_count_max=2\n"
            << "ssh_batch_mode=yes\n"
            << "ssh_strict_host_key=accept-new\n\n"

            << "# --- options ---\n"
            << "run_local_worker_on_master=true\n\n"

            << "# --- workers (examples) ---\n"
            << "# ip,username,worker_path[,ssh_port][,os][,k=v ...]\n"
            << "# keys: auth=key|password, password=..., ssh_client=ssh|plink|sshpass, hostkey=SHA256:..., os=windows|linux|mac\n"
            << "#10.10.10.17,kiinbang,C:\\Workers\\Test_master_worker.exe,22,windows,auth=key,ssh_client=ssh,hostkey=SHA256:YOUR_HOSTKEY\n"
            << "#10.10.10.18,kiinbang,/Users/kiinbang/Workers/Test_master_worker,22,mac,auth=password,password=MyP@ss,ssh_client=sshpass\n"
            << "#10.10.10.19,kiinbang,/home/kiinbang/Workers/Test_master_worker,22,linux,auth=password,password=MyP@ss,ssh_client=sshpass\n"
            << "#10.10.10.21,kiinbang,C:\\Workers\\Test_master_worker.exe,22,windows,auth=password,password=MyP@ss,ssh_client=plink,hostkey=SHA256:YOUR_HOSTKEY\n";
    }

    static void ensureConfig(const std::string& configFile) {
        std::ifstream file(configFile);
        if (!file.is_open()) {
            createDefaultConfig(configFile);
        }
    }

    static std::vector<RemoteWorkerConfig> loadRemoteWorkers(const std::string& configFile) {
        std::vector<RemoteWorkerConfig> workers;
        std::ifstream file(configFile);

        if (!file.is_open()) {
            std::wcout << L"Config file not found: " << Utils::utf8ToWstring(configFile)
                << L". Running with local workers only.\n";
            return workers;
        }

        std::string line;
        while (std::getline(file, line)) {
            line = Utils::trim(line);
            if (line.empty() || line[0] == '#') continue;

            if (line.find(',') != std::string::npos) {
                RemoteWorkerConfig config = parseWorkerConfig(line);
                if (!config.ipAddress.empty()) {
                    workers.push_back(config);
                    std::wcout << L"Loaded remote worker: " << Utils::utf8ToWstring(config.ipAddress)
                        << L" (" << Utils::utf8ToWstring(config.username) << L")\n";
                }
            }
        }
        return workers;
    }

    static RuntimeSettings loadRuntimeSettings(const std::string& configFile) {
        RuntimeSettings settings;
        std::ifstream file(configFile);
        if (!file.is_open()) return settings;

        std::string line;
        while (std::getline(file, line)) {
            line = Utils::trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.find('=') == std::string::npos || line.find(',') != std::string::npos) continue;

            auto pos = line.find('=');
            std::string key = Utils::toLowerCase(Utils::trim(line.substr(0, pos)));
            std::string value = Utils::trim(line.substr(pos + 1));

            parseSettingValue(key, value, settings);
        }
        return settings;
    }

private:
    static RemoteWorkerConfig parseWorkerConfig(const std::string& line) {
        auto tokens = Utils::split(line, ',');
        if (tokens.size() < 3) return RemoteWorkerConfig();

        RemoteWorkerConfig config;
        config.ipAddress = Utils::trim(tokens[0]);
        config.username = Utils::trim(tokens[1]);
        config.workerPath = Utils::trim(tokens[2]);

        if (tokens.size() >= 4 && !Utils::trim(tokens[3]).empty()) {
            try {
                config.sshPort = std::stoi(Utils::trim(tokens[3]));
            }
            catch (...) {
                // Keep default port
            }
        }

        // Parse additional tokens (k=v pairs or legacy OS token)
        for (size_t i = 4; i < tokens.size(); ++i) {
            std::string token = Utils::trim(tokens[i]);
            if (token.empty()) continue;

            auto keyValue = Utils::split(token, '=');
            if (keyValue.size() != 2) {
                if (i == 4) config.osHint = Utils::toLowerCase(token);
                continue;
            }

            std::string key = Utils::toLowerCase(Utils::trim(keyValue[0]));
            std::string value = Utils::trim(keyValue[1]);

            if (key == "auth") config.authMethod = Utils::toLowerCase(value);
            else if (key == "password") config.password = value;
            else if (key == "ssh_client") config.sshClient = Utils::toLowerCase(value);
            else if (key == "hostkey") config.hostkey = value;
            else if (key == "os") config.osHint = Utils::toLowerCase(value);
        }

        return config;
    }

    static void parseSettingValue(const std::string& key, const std::string& value,
        RuntimeSettings& settings) {
        try {
            if (key == "drain_seconds") {
                settings.drainSeconds = std::stoi(value);
            }
            else if (key == "worker_idle_timeout_seconds") {
                settings.workerIdleTimeoutSeconds = std::stoi(value);
            }
            else if (key == "worker_retry_max") {
                settings.workerRetryMax = std::stoi(value);
            }
            else if (key == "worker_retry_backoff_ms") {
                settings.workerRetryBackoffMs = std::stoi(value);
            }
            else if (key == "worker_recv_timeout_ms") {
                settings.workerRecvTimeoutMs = std::stoi(value);
            }
            else if (key == "worker_send_timeout_ms") {
                settings.workerSendTimeoutMs = std::stoi(value);
            }
            else if (key == "ssh_connect_timeout_sec") {
                settings.sshConnectTimeoutSec = std::stoi(value);
            }
            else if (key == "ssh_server_alive_interval_sec") {
                settings.sshServerAliveIntervalSec = std::stoi(value);
            }
            else if (key == "ssh_server_alive_count_max") {
                settings.sshServerAliveCountMax = std::stoi(value);
            }
            else if (key == "ssh_batch_mode") {
                settings.sshBatchMode = Utils::parseBool(value);
            }
            else if (key == "ssh_strict_host_key") {
                settings.sshStrictHostKey = value;
            }
            else if (key == "run_local_worker_on_master") {
                settings.runLocalWorkerOnMaster = Utils::parseBool(value);
            }
            else if (key == "master_starvation_seconds") {
                settings.masterStarvationSeconds = std::stoi(value);
            }
            else if (key == "emergency_local_spawn_max") {
                settings.emergencyLocalSpawnMax = std::stoi(value);
            }
        }
        catch (...) {
            std::wcout << L"[Config] Failed to parse key: " << Utils::utf8ToWstring(key) << L"\n";
        }
    }
};

///////////////////////////////////////////////////////////////////////////////
// Point Cloud Data Structures
///////////////////////////////////////////////////////////////////////////////

struct Point3D {
    float x, y, z;

    Point3D(float x = 0, float y = 0, float z = 0) : x(x), y(y), z(z) {}
};

struct PointCloudChunk {
    int chunkId = 0;
    std::vector<Point3D> points;

    std::string serialize() const {
        std::stringstream ss;
        ss << chunkId << " " << points.size() << " ";

        for (const auto& point : points) {
            ss << point.x << " " << point.y << " " << point.z << " ";
        }
        return ss.str();
    }

    static PointCloudChunk deserialize(const std::string& data) {
        PointCloudChunk chunk;
        std::stringstream ss(data);
        size_t pointCount = 0;

        ss >> chunk.chunkId >> pointCount;
        chunk.points.reserve(pointCount);

        for (size_t i = 0; i < pointCount; ++i) {
            Point3D point;
            ss >> point.x >> point.y >> point.z;
            chunk.points.push_back(point);
        }
        return chunk;
    }
};

struct ProcessResult {
    int chunkId = 0;
    float avgDistance = 0.0f;
    int pointCount = 0;

    std::string serialize() const {
        std::stringstream ss;
        ss << chunkId << " " << avgDistance << " " << pointCount;
        return ss.str();
    }

    static ProcessResult deserialize(const std::string& data) {
        ProcessResult result;
        std::stringstream ss(data);
        ss >> result.chunkId >> result.avgDistance >> result.pointCount;
        return result;
    }
};

///////////////////////////////////////////////////////////////////////////////
// Remote Worker Management
///////////////////////////////////////////////////////////////////////////////

class RemoteWorkerManager {
private:
    std::vector<RemoteWorkerConfig> remoteConfigs;
    std::vector<std::thread> remoteThreads;
    std::string masterIP;
    int masterPort;
    RuntimeSettings settings;
    std::atomic<bool> shouldStop{ false };

public:
    RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
        const std::string& masterIP, int masterPort,
        const RuntimeSettings& settings)
        : remoteConfigs(configs), masterIP(masterIP), masterPort(masterPort), settings(settings) {
    }

    ~RemoteWorkerManager() {
        stop();
    }

    void startRemoteWorkers() {
        for (const auto& config : remoteConfigs) {
            remoteThreads.emplace_back([this, config]() {
                this->runRemoteWorker(config);
                });
        }
        std::wcout << L"Started " << remoteConfigs.size() << L" remote workers\n";
    }

    void stop() {
        shouldStop.store(true);
        for (auto& thread : remoteThreads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        remoteThreads.clear();
    }

private:
    void runRemoteWorker(const RemoteWorkerConfig& config) {
        std::wcout << L"Starting remote worker on " << Utils::utf8ToWstring(config.ipAddress) << L"\n";

        std::string sshCommand = buildSSHCommand(config);
        std::wcout << L"Executing: " << Utils::utf8ToWstring(sshCommand) << L"\n";

        while (!shouldStop.load()) {
            int returnCode = std::system(sshCommand.c_str());

            if (returnCode == 0) {
                std::wcout << L"Remote worker on " << Utils::utf8ToWstring(config.ipAddress)
                    << L" completed successfully\n";
            }
            else {
                std::wcout << L"Remote worker on " << Utils::utf8ToWstring(config.ipAddress)
                    << L" failed or disconnected (code: " << returnCode << L")\n";
            }

            if (!shouldStop.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }

        std::wcout << L"Remote worker thread for " << Utils::utf8ToWstring(config.ipAddress)
            << L" terminated\n";
    }

    std::string buildSSHCommand(const RemoteWorkerConfig& config) {
        const bool usePassword = (Utils::toLowerCase(config.authMethod) == "password");

#ifdef _WIN32
        return buildWindowsSSHCommand(config, usePassword);
#else
        return buildUnixSSHCommand(config, usePassword);
#endif
    }

#ifdef _WIN32
    std::string buildWindowsSSHCommand(const RemoteWorkerConfig& config, bool usePassword) {
        const std::string client = !config.sshClient.empty() ?
            config.sshClient :
            (usePassword ? "plink" : "ssh");

        std::stringstream command;

        if (client == "plink") {
            command << "plink -ssh -P " << config.sshPort
                << " -l " << config.username;

            if (usePassword && !config.password.empty()) {
                command << " -pw \"" << Utils::escapeDQ(config.password) << "\"";
            }

            command << " -batch";

            if (!config.hostkey.empty()) {
                command << " -hostkey \"" << Utils::escapeDQ(config.hostkey) << "\"";
            }

            command << " " << config.ipAddress
                << " \"" << Utils::escapeDQ(config.workerPath)
                << " worker " << masterIP << " " << masterPort << " workers.conf\"";
        }
        else {
            command << "ssh -p " << config.sshPort
                << " -o ConnectTimeout=" << settings.sshConnectTimeoutSec
                << " -o ServerAliveInterval=" << settings.sshServerAliveIntervalSec
                << " -o ServerAliveCountMax=" << settings.sshServerAliveCountMax
                << " -o StrictHostKeyChecking=" << settings.sshStrictHostKey;

            if (settings.sshBatchMode && !usePassword) {
                command << " -o BatchMode=yes";
            }

            command << " " << config.username << "@" << config.ipAddress
                << " \"" << Utils::escapeDQ(config.workerPath)
                << " worker " << masterIP << " " << masterPort << " workers.conf\""
                << " < NUL";
        }

        return command.str();
    }
#else
    std::string buildUnixSSHCommand(const RemoteWorkerConfig& config, bool usePassword) {
        const std::string client = !config.sshClient.empty() ?
            config.sshClient :
            (usePassword ? "sshpass" : "ssh");

        std::stringstream command;

        if (client == "sshpass" && usePassword) {
            command << "sshpass -p \"" << Utils::escapeDQ(config.password) << "\" "
                << "ssh -p " << config.sshPort
                << " -o StrictHostKeyChecking=" << settings.sshStrictHostKey
                << " -o ConnectTimeout=" << settings.sshConnectTimeoutSec
                << " -o ServerAliveInterval=" << settings.sshServerAliveIntervalSec
                << " -o ServerAliveCountMax=" << settings.sshServerAliveCountMax
                << " -n "
                << config.username << "@" << config.ipAddress
                << " '" << config.workerPath
                << " worker " << masterIP << " " << masterPort << " workers.conf'";
        }
        else {
            command << "ssh -p " << config.sshPort
                << " -o StrictHostKeyChecking=" << settings.sshStrictHostKey
                << " -o ConnectTimeout=" << settings.sshConnectTimeoutSec
                << " -o ServerAliveInterval=" << settings.sshServerAliveIntervalSec
                << " -o ServerAliveCountMax=" << settings.sshServerAliveCountMax
                << " -n";

            if (settings.sshBatchMode) {
                command << " -o BatchMode=yes";
            }

            command << " " << config.username << "@" << config.ipAddress
                << " '" << config.workerPath
                << " worker " << masterIP << " " << masterPort << " workers.conf'";
        }

        return command.str();
    }
#endif
};

///////////////////////////////////////////////////////////////////////////////
// Master Server Implementation
///////////////////////////////////////////////////////////////////////////////

class MasterServer {
private:
    // Core data
    std::vector<PointCloudChunk> chunks;
    std::vector<ProcessResult> results;
    std::atomic<int> chunkIndex{ 0 };
    std::mutex resultsMutex;
    int port;
    std::atomic<bool> allChunksProcessed{ false };

    // Configuration
    std::unique_ptr<RemoteWorkerManager> remoteManager;
    std::string configFile;
    RuntimeSettings settings;
    std::vector<RemoteWorkerConfig> remoteConfigs;

    // Work management
    std::deque<int> retryQueue;
    std::mutex retryMutex;
    std::unordered_set<int> completedChunkIds;
    std::chrono::steady_clock::time_point lastProgress{ std::chrono::steady_clock::now() };
    int emergencyLocalSpawned = 0;

public:
    MasterServer(int port, const std::string& configFile = "workers.conf")
        : port(port), configFile(configFile) {
    }

    ~MasterServer() {
        if (remoteManager) {
            remoteManager->stop();
        }
    }

    void generateSampleData() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

        for (int chunkId = 0; chunkId < 100; ++chunkId) {
            PointCloudChunk chunk;
            chunk.chunkId = chunkId;

            for (int i = 0; i < 1000; ++i) {
                chunk.points.emplace_back(dist(gen), dist(gen), dist(gen));
            }

            chunks.push_back(std::move(chunk));
        }

        std::wcout << L"Generated " << chunks.size() << L" chunks with total "
            << (chunks.size() * 1000) << L" points\n";
    }

    void start() {
        initializeConfiguration();
        startLocalWorkerIfNeeded();
        setupRemoteWorkers();
        runServerLoop();
        printFinalResults();
        shutdownRemoteWorkers();
    }

private:
    void initializeConfiguration() {
        ConfigManager::ensureConfig(configFile);
        settings = ConfigManager::loadRuntimeSettings(configFile);
        remoteConfigs = ConfigManager::loadRemoteWorkers(configFile);
    }

    void startLocalWorkerIfNeeded() {
        if (!settings.runLocalWorkerOnMaster) return;

        const std::string selfExe = System::getSelfExecutablePath();
        int returnCode = System::startLocalWorkerNewWindow(selfExe, "127.0.0.1", port);

#ifdef _WIN32
        if (returnCode != 0) {
            std::wcerr << L"[WARN ] Local worker start failed (rc=" << returnCode << L")\n";
        }
        else {
            std::wcout << L"[ OK  ] Local worker started in a new window\n";
        }
#else
        if (returnCode != 0) {
            std::cerr << "[WARN ] Local worker start failed (rc=" << returnCode << ")\n";
        }
        else {
            std::cout << "[ OK  ] Local worker started in a new window\n";
        }
#endif
    }

    void setupRemoteWorkers() {
        if (remoteConfigs.empty()) {
            std::wcout << L"No remote workers configured. Running with local workers only.\n";
            return;
        }

        std::string masterIP = Network::getLocalIPAddress();
        remoteManager = std::make_unique<RemoteWorkerManager>(remoteConfigs, masterIP, port, settings);

        std::wcout << L"Starting remote workers (Master IP: " << Utils::utf8ToWstring(masterIP)
            << L":" << port << L")...\n";

        remoteManager->startRemoteWorkers();
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    void runServerLoop() {
        SOCKET serverSocket = createAndBindSocket();
        if (serverSocket == INVALID_SOCKET) return;

        std::wcout << L"Master server listening on port " << port << L"\n";

        bool draining = false;
        auto drainDeadline = (std::chrono::steady_clock::time_point::max)();

        while (true) {
            if (handleIncomingConnections(serverSocket, draining, drainDeadline)) {
                break; // Exit condition met
            }
        }

        closesocket(serverSocket);
    }

    SOCKET createAndBindSocket() {
        SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket == INVALID_SOCKET) {
            std::wcerr << L"Socket creation failed\n";
            return INVALID_SOCKET;
        }

        int opt = 1;
        setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (bind(serverSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::wcerr << L"Bind failed\n";
            closesocket(serverSocket);
            return INVALID_SOCKET;
        }

        if (listen(serverSocket, 10) == SOCKET_ERROR) {
            std::wcerr << L"Listen failed\n";
            closesocket(serverSocket);
            return INVALID_SOCKET;
        }

        return serverSocket;
    }

    bool handleIncomingConnections(SOCKET serverSocket, bool& draining,
        std::chrono::steady_clock::time_point& drainDeadline) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(serverSocket, &readFds);

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

#ifdef _WIN32
        int ready = select(0, &readFds, nullptr, nullptr, &timeout);
#else
        int ready = select(serverSocket + 1, &readFds, nullptr, nullptr, &timeout);
#endif

        auto now = std::chrono::steady_clock::now();

        if (ready <= 0) {
            handleIdleTime(now, draining);
            return draining && now >= drainDeadline;
        }

        // Accept new connection
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(serverSocket,
            reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);

        if (clientSocket == INVALID_SOCKET) return false;

        char ipStr[INET_ADDRSTRLEN] = { 0 };
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, INET_ADDRSTRLEN);
        std::wcout << L"Worker connected: " << Utils::utf8ToWstring(ipStr) << L"\n";

        bool workProcessed = handleWorkerConnection(clientSocket, draining);
        if (workProcessed) {
            lastProgress = std::chrono::steady_clock::now();
        }

        closesocket(clientSocket);

        // Check if we should start draining
        if (!draining && shouldStartDraining()) {
            draining = true;
            drainDeadline = now + std::chrono::seconds(settings.drainSeconds);
            std::wcout << L"All chunks processed! Entering draining phase for "
                << settings.drainSeconds << L"s...\n";
        }

        return false;
    }

    void handleIdleTime(std::chrono::steady_clock::time_point now, bool draining) {
        if (!draining && hasPendingWork()) {
            auto idleSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                now - lastProgress).count();

            if (idleSeconds >= settings.masterStarvationSeconds) {
                spawnEmergencyLocalWorkerIfNeeded(now);
            }
        }
    }

    void spawnEmergencyLocalWorkerIfNeeded(std::chrono::steady_clock::time_point now) {
        if (settings.runLocalWorkerOnMaster &&
            emergencyLocalSpawned < settings.emergencyLocalSpawnMax) {

            const std::string selfExe = System::getSelfExecutablePath();
            System::startLocalWorkerNewWindow(selfExe, "127.0.0.1", port);
            ++emergencyLocalSpawned;
            lastProgress = now;
        }
    }

    bool handleWorkerConnection(SOCKET clientSocket, bool draining) {
        int chunkId;
        bool hasWork = (!draining) && tryAcquireNextChunk(chunkId);

        if (!hasWork) {
            Network::NetworkUtils::sendTerminationSignal(clientSocket);
            std::wcout << L"Sent termination signal to worker\n";
            return false;
        }

        return processWorkerRequest(clientSocket, chunkId);
    }

    bool processWorkerRequest(SOCKET clientSocket, int chunkId) {
        // Send chunk to worker
        std::string chunkData = chunks[chunkId].serialize();
        bool sentOk = Network::NetworkUtils::sendData(clientSocket, chunkData);

        if (!sentOk) {
            std::wcout << L"[WARN ] Failed to send chunk " << chunkId
                << L" to worker. Requeue.\n";
            requeueChunkIfNeeded(chunkId);
            return false;
        }

        std::wcout << L"Sent chunk " << chunkId << L" to worker\n";

        // Receive result from worker
        std::string resultData = Network::NetworkUtils::receiveData(clientSocket);
        if (resultData.empty()) {
            std::wcout << L"[WARN ] Worker disconnected before sending result for chunk "
                << chunkId << L". Requeue.\n";
            requeueChunkIfNeeded(chunkId);
            return false;
        }

        return processWorkerResult(resultData);
    }

    bool processWorkerResult(const std::string& resultData) {
        ProcessResult result = ProcessResult::deserialize(resultData);
        bool isNewResult = false;

        {
            std::lock_guard<std::mutex> lock(resultsMutex);
            if (completedChunkIds.insert(result.chunkId).second) {
                results.push_back(result);
                isNewResult = true;
                std::wcout << L"Received result for chunk " << result.chunkId
                    << L": avg_distance=" << result.avgDistance
                    << L", points=" << result.pointCount << L"\n";
            }
            else {
                std::wcout << L"[INFO ] Duplicate result ignored for chunk "
                    << result.chunkId << L"\n";
            }
        }

        return isNewResult;
    }

    bool hasPendingWork() const {
        return completedChunkIds.size() < chunks.size();
    }

    bool shouldStartDraining() const {
        std::lock_guard<std::mutex> lock(resultsMutex);
        return completedChunkIds.size() == chunks.size() && !allChunksProcessed.load();
    }

    bool tryAcquireNextChunk(int& outChunkId) {
        std::lock_guard<std::mutex> lock(retryMutex);

        // First, try retry queue
        if (!retryQueue.empty()) {
            outChunkId = retryQueue.front();
            retryQueue.pop_front();
            return true;
        }

        // Then try new chunks
        int index = chunkIndex.fetch_add(1);
        if (index < static_cast<int>(chunks.size())) {
            outChunkId = index;
            return true;
        }

        return false;
    }

    void requeueChunkIfNeeded(int chunkId) {
        std::lock_guard<std::mutex> lock(retryMutex);
        if (completedChunkIds.find(chunkId) == completedChunkIds.end()) {
            retryQueue.push_back(chunkId);
        }
    }

    void printFinalResults() {
        std::wcout << L"\n=== Final Results ===\n";

        float totalAvg = 0.0f;
        int totalPoints = 0;

        for (const auto& result : results) {
            totalAvg += result.avgDistance;
            totalPoints += result.pointCount;
            std::wcout << L"Chunk " << result.chunkId << L": " << result.pointCount
                << L" points, avg_distance=" << result.avgDistance << L"\n";
        }

        if (!results.empty()) {
            std::wcout << L"Overall average distance: " << (totalAvg / results.size()) << L"\n";
            std::wcout << L"Total points processed: " << totalPoints << L"\n";
        }
    }

    void shutdownRemoteWorkers() {
        if (remoteManager) {
            std::wcout << L"Shutting down remote workers...\n";
            remoteManager->stop();
        }
    }
};

///////////////////////////////////////////////////////////////////////////////
// Worker Client Implementation
///////////////////////////////////////////////////////////////////////////////

class WorkerClient {
private:
    std::string masterIP;
    int masterPort;
    RuntimeSettings settings;
    bool lastWasTerminate = false;

public:
    WorkerClient(const std::string& ip, int port, const RuntimeSettings& settings)
        : masterIP(ip), masterPort(port), settings(settings) {
    }

    void start() {
        using Clock = std::chrono::steady_clock;
        auto lastSuccess = Clock::now();
        int failCount = 0;

        std::wcout << L"Worker started. Connecting to master...\n";

        while (true) {
            if (!connectAndProcess()) {
                if (lastWasTerminate) {
                    std::wcout << L"Worker terminated by master.\n";
                    break;
                }

                if (shouldExitWorker(lastSuccess, failCount)) {
                    std::wcout << L"No more work or no response from master. Exiting.\n";
                    break;
                }

                int backoffMs = settings.workerRetryBackoffMs * failCount;
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                continue;
            }

            // Reset on success
            failCount = 0;
            lastSuccess = Clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

private:
    bool shouldExitWorker(std::chrono::steady_clock::time_point lastSuccess, int& failCount) {
        auto idleElapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - lastSuccess).count();

        return (idleElapsed >= settings.workerIdleTimeoutSeconds ||
            ++failCount >= settings.workerRetryMax);
    }

    bool connectAndProcess() {
        SOCKET socket = createAndConfigureSocket();
        if (socket == INVALID_SOCKET) return false;

        if (!connectToMaster(socket)) {
            closesocket(socket);
            return false;
        }

        bool success = handleMasterCommunication(socket);
        closesocket(socket);
        return success;
    }

    SOCKET createAndConfigureSocket() {
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket == INVALID_SOCKET) {
            std::wcerr << L"Socket creation failed\n";
            return INVALID_SOCKET;
        }

        setSocketTimeouts(socket);
        return socket;
    }

    void setSocketTimeouts(SOCKET socket) {
#ifdef _WIN32
        DWORD recvTimeout = settings.workerRecvTimeoutMs;
        DWORD sendTimeout = settings.workerSendTimeoutMs;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&recvTimeout), sizeof(recvTimeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
#else
        auto msToTimeval = [](int ms) {
            timeval tv{};
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            return tv;
            };

        timeval recvTimeout = msToTimeval(settings.workerRecvTimeoutMs);
        timeval sendTimeout = msToTimeval(settings.workerSendTimeoutMs);
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
#endif
    }

    bool connectToMaster(SOCKET socket) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(masterPort);

        if (inet_pton(AF_INET, masterIP.c_str(), &addr.sin_addr) != 1) {
            std::wcerr << L"Invalid IP address\n";
            return false;
        }

        if (::connect(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::wcerr << L"Connection to master failed\n";
            return false;
        }

        return true;
    }

    bool handleMasterCommunication(SOCKET socket) {
        // Receive data from master
        std::string receivedData = Network::NetworkUtils::receiveData(socket);
        if (receivedData.empty()) {
            std::wcerr << L"Failed to receive data from master\n";
            lastWasTerminate = false;
            return false;
        }

        // Check for termination signal
        if (Network::NetworkUtils::isTerminationSignal(receivedData)) {
            std::wcout << L"Received termination signal from master\n";
            lastWasTerminate = true;
            return false;
        }

        // Process the chunk
        PointCloudChunk chunk = PointCloudChunk::deserialize(receivedData);
        std::wcout << L"Received chunk " << chunk.chunkId << L" with "
            << chunk.points.size() << L" points\n";

        ProcessResult result = processPointCloud(chunk);

        // Send result back
        Network::NetworkUtils::sendData(socket, result.serialize());
        std::wcout << L"Sent processing result for chunk " << chunk.chunkId << L"\n";

        lastWasTerminate = false;
        return true;
    }

    ProcessResult processPointCloud(const PointCloudChunk& chunk) {
        std::wcout << L"Processing chunk " << chunk.chunkId << L"...\n";

        float totalDistance = 0.0f;
        for (const auto& point : chunk.points) {
            float distance = std::sqrt(point.x * point.x + point.y * point.y + point.z * point.z);
            totalDistance += distance;
        }

        // Simulate processing time
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        ProcessResult result;
        result.chunkId = chunk.chunkId;
        result.avgDistance = chunk.points.empty() ?
            0.0f : totalDistance / static_cast<float>(chunk.points.size());
        result.pointCount = static_cast<int>(chunk.points.size());

        std::wcout << L"Finished processing chunk " << chunk.chunkId
            << L" (avg_distance: " << result.avgDistance << L")\n";

        return result;
    }
};

///////////////////////////////////////////////////////////////////////////////
// Main Application Entry Point
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_U16TEXT);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::wcerr << L"WSAStartup failed\n";
        return 1;
    }

    if (argc < 2) {
        std::wcout << L"Usage:\n"
            << L"  Master mode: " << argv[0] << L" master [port] [config_file]\n"
            << L"  Worker mode: " << argv[0] << L" worker [master_ip] [master_port] [config_file]\n";
        WSACleanup();
        return 1;
    }

    std::wstring mode = argv[1];

    if (mode == L"master") {
        int port = (argc >= 3) ? _wtoi(argv[2]) : 8080;
        std::string configFile = (argc >= 4) ? Utils::wstringToUtf8(argv[3]) : "workers.conf";

        MasterServer server(port, configFile);
        server.generateSampleData();
        server.start();

    }
    else if (mode == L"worker") {
        std::wstring masterIPWide = (argc >= 3) ? argv[2] : L"127.0.0.1";
        int masterPort = (argc >= 4) ? _wtoi(argv[3]) : 8080;
        std::string configFile = (argc >= 5) ? Utils::wstringToUtf8(argv[4]) : "workers.conf";

        ConfigManager::ensureConfig(configFile);
        RuntimeSettings settings = ConfigManager::loadRuntimeSettings(configFile);

        WorkerClient worker(Utils::wstringToUtf8(masterIPWide), masterPort, settings);
        worker.start();

    }
    else {
        std::wcerr << L"Invalid mode. Use 'master' or 'worker'\n";
        WSACleanup();
        return 1;
    }

    WSACleanup();
    return 0;
}

#else

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n"
            << "  Master mode: " << argv[0] << " master [port] [config_file]\n"
            << "  Worker mode: " << argv[0] << " worker [master_ip] [master_port] [config_file]\n";
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "master") {
        int port = (argc >= 3) ? std::stoi(argv[2]) : 8080;
        std::string configFile = (argc >= 4) ? argv[3] : "workers.conf";

        MasterServer server(port, configFile);
        server.generateSampleData();
        server.start();

    }
    else if (mode == "worker") {
        std::string masterIP = (argc >= 3) ? argv[2] : "127.0.0.1";
        int masterPort = (argc >= 4) ? std::stoi(argv[3]) : 8080;
        std::string configFile = (argc >= 5) ? argv[4] : "workers.conf";

        ConfigManager::ensureConfig(configFile);
        RuntimeSettings settings = ConfigManager::loadRuntimeSettings(configFile);

        WorkerClient worker(masterIP, masterPort, settings);
        worker.start();

    }
    else {
        std::cerr << "Invalid mode. Use 'master' or 'worker'\n";
        return 1;
    }

    return 0;
}

#endif