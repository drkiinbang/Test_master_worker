#pragma once
#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <memory>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <thread>
#include <chrono>

// Silence deprecation warnings for <codecvt> on MSVC
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING 1

#include <locale>
#include <codecvt>

// Cross platform headers related with socket/network
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

// Wide characters for Windows console
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
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
static std::string escapeDQ(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) out += (c == '"') ? "\\\"" : std::string(1, c);
    return out;
}

/// Point / Chunk / Result
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

/// === Self executable absolute path ===
std::string getSelfExePath();

/// 로컬 IP 자동 탐지(IPv4)
std::string getLocalIPAddress(); /// static 키워드는 해당 함수의 가시성을 정의된 파일 내부로 제한함