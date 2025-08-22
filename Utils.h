#pragma once

#include <codecvt>

/// Simple quote-escape helper for shell-safe command building (macOS/Linux)
static std::string escapeDQ(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) out += (c == '"') ? "\\\"" : std::string(1, c);
    return out;
}

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