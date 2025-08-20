/// ============================================================================
/// main.cpp — Cross-Platform Remote Worker Launcher (새 콘솔/터미널 창 실행)
/// ----------------------------------------------------------------------------
/// 기능 요약:
/// 1) workers.conf(CSV) 읽기: ip, username, worker_path[,ssh_port][,os]
/// 2) OS별로 "새 창"에서 워커 실행:
///    - Windows:  cmd /c start "" "<exe>" args...
///    - macOS:    osascript 로 Terminal.app 새 창에서 bash -lc "<cmd>; exec bash -i"
///    - Linux:    gnome-terminal/konsole/xterm 탐지 후 새 창 실행, 미설치 시 tmux fallback
/// 3) SSH 옵션(기본값):
///    - ConnectTimeout=5, ServerAliveInterval=5, ServerAliveCountMax=2
///    - StrictHostKeyChecking=accept-new, BatchMode=yes
///    - 호스트(WINDOWS): 원격 명령 "..." 로 감싸고 < NUL 로 stdin 분리
///    - 호스트(POSIX):  원격 명령 '...' 로 감싸고 -n 으로 stdin 분리
/// 4) master IP 자동 감지(옵션): 인자 미제공 시 UDP 소켓으로 로컬 IP 추정
/// 5) SSH 개인키 지정(옵션): 환경변수 SSH_IDENTITY 로 -i "경로" 자동 추가
///
/// 사용법:
///   Test_master_worker master <port> <workers.conf>
///   또는
///   Test_master_worker master <master_ip> <port> <workers.conf>
///
/// 예)
///   Test_master_worker master 8080 workers.conf
///   Test_master_worker master 10.10.10.13 8080 workers.conf
///
/// 주의:
/// - 원격에 "진짜 화면"에 보여야 하면(Windows) schtasks /IT 또는 psexec -i 사용 권장.
/// - Linux/macOS 는 GUI 로그인 세션이 있어야 새 터미널 창이 화면에 표시됨.
/// ============================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>
#include <algorithm>
#include <cstdlib>

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

/// ----------------------------------------------------------------------------
/// 유틸: 문자열 trim
/// ----------------------------------------------------------------------------
static inline std::string ltrim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    return s;
}
static inline std::string rtrim(std::string s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
    return s;
}
static inline std::string trim(std::string s) { return rtrim(ltrim(std::move(s))); }

/// ----------------------------------------------------------------------------
/// 원격 OS 힌트
/// ----------------------------------------------------------------------------
enum class RemoteOS { Windows, Mac, Linux, Unknown };

/// ----------------------------------------------------------------------------
/// workers.conf 구조체
/// ----------------------------------------------------------------------------
struct RemoteWorkerConfig {
    std::string ip;
    std::string username;
    std::string worker_path;
    int ssh_port = 22;
    RemoteOS os_hint = RemoteOS::Unknown;
};

/// ----------------------------------------------------------------------------
/// 안전한 환경변수 조회 (Windows: _dupenv_s, POSIX: getenv)
/// ----------------------------------------------------------------------------
static bool getEnvVar(const char* name, std::string& out) {
#ifdef _WIN32
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) {
        if (len > 0) out.assign(buf, len - 1); else out.clear();
        free(buf);
        return true;
    }
    return false;
#else
    const char* v = std::getenv(name);
    if (v) { out = v; return true; }
    return false;
#endif
}

/// ----------------------------------------------------------------------------
/// 더블쿼트 내부 이스케이프
/// ----------------------------------------------------------------------------
static std::string escapeForDoubleQuotes(const std::string& s) {
    std::string out; out.reserve(s.size() * 2);
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

/// ----------------------------------------------------------------------------
/// 경로로 Windows 여부 추론
/// ----------------------------------------------------------------------------
static bool isLikelyWindowsPath(const std::string& p) {
    if (p.size() > 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && (p[2] == '\\' || p[2] == '/'))
        return true;
    return p.find('\\') != std::string::npos;
}

/// ----------------------------------------------------------------------------
/// workers.conf 파서 (CSV: ip,username,worker_path[,ssh_port][,os])
/// ----------------------------------------------------------------------------
static std::vector<RemoteWorkerConfig> parseWorkersConf(const std::string& path) {
    std::vector<RemoteWorkerConfig> out;
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << "[ERROR] Cannot open workers.conf: " << path << "\n";
        return out;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(ifs, line)) {
        ++lineno;
        std::string raw = trim(line);
        if (raw.empty()) continue;
        if (raw[0] == '#') continue;

        // CSV split (단순 쉼표 기준)
        std::vector<std::string> fields;
        {
            std::stringstream ss(raw);
            std::string tok;
            while (std::getline(ss, tok, ',')) fields.push_back(trim(tok));
        }
        if (fields.size() < 3) {
            std::cerr << "[WARN] Invalid line " << lineno << ": need >=3 fields\n";
            continue;
        }
        RemoteWorkerConfig cfg;
        cfg.ip = fields[0];
        cfg.username = fields[1];
        cfg.worker_path = fields[2];

        if (fields.size() >= 4 && !fields[3].empty()) {
            try { cfg.ssh_port = std::stoi(fields[3]); }
            catch (...) { std::cerr << "[WARN] Invalid ssh_port at line " << lineno << ", default 22\n"; }
        }
        else {
            cfg.ssh_port = 22;
        }

        if (fields.size() >= 5 && !fields[4].empty()) {
            std::string os = fields[4];
            std::transform(os.begin(), os.end(), os.begin(), [](unsigned char c) { return std::tolower(c); });
            if (os == "windows" || os == "win") cfg.os_hint = RemoteOS::Windows;
            else if (os == "mac" || os == "darwin" || os == "osx") cfg.os_hint = RemoteOS::Mac;
            else if (os == "linux") cfg.os_hint = RemoteOS::Linux;
            else cfg.os_hint = RemoteOS::Unknown;
        }
        else {
            cfg.os_hint = isLikelyWindowsPath(cfg.worker_path) ? RemoteOS::Windows : RemoteOS::Unknown;
        }
        out.push_back(std::move(cfg));
    }
    return out;
}

/// ----------------------------------------------------------------------------
/// 워커 프로세스 실행 기본 커맨드 생성
/// ----------------------------------------------------------------------------
static std::string buildBaseWorkerCmd(const std::string& workerPath,
    const std::string& masterIp,
    int masterPort,
    const std::string& workerConf /*e.g., "workers.conf"*/) {
    std::ostringstream oss;
    oss << "\"" << workerPath << "\""
        << " worker " << masterIp << " " << masterPort << " " << workerConf;
    return oss.str();
}

/// ----------------------------------------------------------------------------
/// OS별 새 창에서 실행하는 래퍼 커맨드
/// ----------------------------------------------------------------------------
static std::string buildWindowsNewConsole(const std::string& baseCmd) {
    // 새 콘솔: start "" "<exe>" args...
    return "cmd /c start \"\" " + baseCmd;
}

static std::string buildMacNewTerminal(const std::string& baseCmd) {
    // Terminal.app: bash -lc "<cmd>; exec bash -i"
    const std::string bashLine = "bash -lc \\\"" + escapeForDoubleQuotes(baseCmd) + "; exec bash -i\\\"";
    std::ostringstream oss;
    oss << "osascript -e 'tell application \"Terminal\" to do script \"" << bashLine << "\"' "
        << "-e 'tell application \"Terminal\" to activate'";
    return oss.str();
}

static std::string buildLinuxNewTerminal(const std::string& baseCmd) {
    // 우선순위: gnome-terminal -> konsole -> xterm -> tmux fallback
    const std::string inner =
        "if [ -z \"$DISPLAY\" ]; then export DISPLAY=:0; fi; "
        "if command -v gnome-terminal >/dev/null 2>&1; then "
        "gnome-terminal -- bash -lc \"" + escapeForDoubleQuotes(baseCmd) + "; exec bash -i\"; "
        "elif command -v konsole >/dev/null 2>&1; then "
        "konsole --hold -e bash -lc \"" + escapeForDoubleQuotes(baseCmd) + "; exec bash -i\"; "
        "elif command -v xterm >/dev/null 2>&1; then "
        "xterm -hold -e \"" + escapeForDoubleQuotes(baseCmd) + "\"; "
        "else "
        "tmux new-session -d -s pointcloud \"" + escapeForDoubleQuotes(baseCmd) + "\"; "
        "echo \"No desktop terminal found; started in tmux session 'pointcloud'\"; "
        "fi";
    return "bash -lc \"" + escapeForDoubleQuotes(inner) + "\"";
}

static std::string buildPosixSmartTerminal(const std::string& baseCmd) {
    // macOS/리눅스 자동: osascript 존재하면 mac 경로, 아니면 Linux 터미널 시도
    const std::string inner =
        "if command -v osascript >/dev/null 2>&1; then "
        "osascript -e 'tell application \"Terminal\" to do script \"bash -lc \\\\\\\""
        + escapeForDoubleQuotes(baseCmd) +
        "; exec bash -i\\\\\\\"\"' "
        "-e 'tell application \"Terminal\" to activate'; "
        "else "
        "if [ -z \"$DISPLAY\" ]; then export DISPLAY=:0; fi; "
        "if command -v gnome-terminal >/dev/null 2>&1; then "
        "gnome-terminal -- bash -lc \"" + escapeForDoubleQuotes(baseCmd) + "; exec bash -i\"; "
        "elif command -v konsole >/dev/null 2>&1; then "
        "konsole --hold -e bash -lc \"" + escapeForDoubleQuotes(baseCmd) + "; exec bash -i\"; "
        "elif command -v xterm >/dev/null 2>&1; then "
        "xterm -hold -e \"" + escapeForDoubleQuotes(baseCmd) + "\"; "
        "else "
        "tmux new-session -d -s pointcloud \"" + escapeForDoubleQuotes(baseCmd) + "\"; "
        "echo \"No desktop terminal found; started in tmux session 'pointcloud'\"; "
        "fi; "
        "fi";
    return "bash -lc \"" + escapeForDoubleQuotes(inner) + "\"";
}

static std::string buildNewWindowCmd(RemoteOS os,
    const std::string& workerPath,
    const std::string& masterIp,
    int masterPort,
    const std::string& workerConf) {
    const std::string baseCmd = buildBaseWorkerCmd(workerPath, masterIp, masterPort, workerConf);
    switch (os) {
    case RemoteOS::Windows: return buildWindowsNewConsole(baseCmd);
    case RemoteOS::Mac:     return buildMacNewTerminal(baseCmd);
    case RemoteOS::Linux:   return buildLinuxNewTerminal(baseCmd);
    case RemoteOS::Unknown: default: return buildPosixSmartTerminal(baseCmd);
    }
}

/// ----------------------------------------------------------------------------
/// 호스트(현재 실행 중인 로컬) OS 판별
/// ----------------------------------------------------------------------------
static bool isHostWindows() {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

/// ----------------------------------------------------------------------------
/// master IP 자동 추정 (UDP 소켓으로 로컬 바인딩 확인)
/// ----------------------------------------------------------------------------
static std::string detectLocalIPv4() {
    std::string fallback = "127.0.0.1";
#if defined(_WIN32)
    WSADATA wsa; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fallback;
#endif
    int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
#if defined(_WIN32)
        WSACleanup();
#endif
        return fallback;
    }
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &serv.sin_addr);

    if (::connect(sockfd, (sockaddr*)&serv, sizeof(serv)) < 0) {
#if defined(_WIN32)
        closesocket(sockfd); WSACleanup();
#else
        ::close(sockfd);
#endif
        return fallback;
    }
    sockaddr_in name{}; socklen_t namelen = sizeof(name);
    if (getsockname(sockfd, (sockaddr*)&name, &namelen) < 0) {
#if defined(_WIN32)
        closesocket(sockfd); WSACleanup();
#else
        ::close(sockfd);
#endif
        return fallback;
    }
    char buf[INET_ADDRSTRLEN] = { 0 };
    const char* p = inet_ntop(AF_INET, &name.sin_addr, buf, sizeof(buf));
#if defined(_WIN32)
    closesocket(sockfd); WSACleanup();
#else
    ::close(sockfd);
#endif
    if (!p) return fallback;
    return std::string(p);
}

/// ----------------------------------------------------------------------------
/// SSH 명령줄 구성 (호스트 OS별 인용/옵션 처리)
/// ----------------------------------------------------------------------------
static std::string buildSshLaunchCommand(const RemoteWorkerConfig& cfg,
    const std::string& masterIp,
    int masterPort,
    const std::string& workerConf) {
    // 원격 새 창 커맨드 구성
    RemoteOS effectiveOS = cfg.os_hint;
    if (effectiveOS == RemoteOS::Unknown) {
        effectiveOS = isLikelyWindowsPath(cfg.worker_path) ? RemoteOS::Windows : RemoteOS::Unknown;
    }
    const std::string remoteCmd = buildNewWindowCmd(effectiveOS, cfg.worker_path, masterIp, masterPort, workerConf);

    // SSH 공통 옵션
    std::ostringstream ssh;
    ssh << "ssh -p " << cfg.ssh_port
        << " -o ConnectTimeout=5"
        << " -o ServerAliveInterval=5"
        << " -o ServerAliveCountMax=2"
        << " -o StrictHostKeyChecking=accept-new"
        << " -o BatchMode=yes";

    // 개인키 지정(있으면)
    {
        std::string key;
        if (getEnvVar("SSH_IDENTITY", key) && !key.empty()) {
            ssh << " -i \"" << escapeForDoubleQuotes(key) << "\"";
        }
    }

    ssh << " " << cfg.username << "@" << cfg.ip << " ";

    if (isHostWindows()) {
        // 원격 명령 전체를 "..." 로 감싸고, 내부 " 이스케이프
        ssh << "\"" << escapeForDoubleQuotes(remoteCmd) << "\""
            << " < NUL";
    }
    else {
        // POSIX 호스트: -n 으로 stdin 분리 + '...'
        ssh << "-n '" << remoteCmd << "'";
    }
    return ssh.str();
}

/// ----------------------------------------------------------------------------
/// 메인: master 모드만 구현 (요청된 패치 핵심에 집중)
/// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage:\n"
            << "  " << argv[0] << " master <port> <workers.conf>\n"
            << "  " << argv[0] << " master <master_ip> <port> <workers.conf>\n";
        return 1;
    }

    std::string mode = argv[1];
    if (mode != "master") {
        std::cerr << "[ERROR] Only 'master' mode is implemented in this patch.\n";
        return 1;
    }

    std::string masterIp;
    int port = 0;
    std::string confPath;

    if (argc == 4) {
        // master <port> <conf>
        port = std::atoi(argv[2]);
        confPath = argv[3];
        masterIp = detectLocalIPv4();
        std::cout << "[INFO ] Master IP auto-detected: " << masterIp << "\n";
    }
    else {
        // master <master_ip> <port> <conf>
        masterIp = argv[2];
        port = std::atoi(argv[3]);
        confPath = argv[4];
    }

    std::cout << "[INFO ] Master server listening on port " << port << "\n";
    std::cout << "[INFO ] Loading workers from: " << confPath << "\n";

    auto workers = parseWorkersConf(confPath);
    if (workers.empty()) {
        std::cerr << "[ERROR] No valid workers found.\n";
        return 1;
    }

    std::cout << "[INFO ] Starting remote workers (Master IP: " << masterIp << ":" << port << ")...\n";
    for (const auto& w : workers) {
        std::string osStr = "unknown";
        switch (w.os_hint) {
        case RemoteOS::Windows: osStr = "windows"; break;
        case RemoteOS::Mac:     osStr = "mac";     break;
        case RemoteOS::Linux:   osStr = "linux";   break;
        default: break;
        }
        std::cout << "[STEP ] Start remote on " << w.ip << " as " << w.username
            << " (ssh:" << w.ssh_port << ", os:" << osStr << ")\n";

        const std::string cmd = buildSshLaunchCommand(w, masterIp, port, confPath);
        std::cout << "[EXEC ] " << cmd << "\n";

        // 실제 실행
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "[WARN ] Remote start failed (rc=" << rc << ") on " << w.ip << "\n";
        }
        else {
            std::cout << "[ OK  ] Remote start triggered on " << w.ip << "\n";
        }
    }

    std::cout << "[DONE ] All remote start commands issued.\n";
    return 0;
}
