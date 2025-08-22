/// 스템 전체의 기반 정의
/// 크로스 플랫폼 헤더 통합(Windows / Linux / macOS)
/// 네트워크 상수 및 오류 코드 정의
/// 핵심 데이터 구조(Point3D, PointCloudChunk, ProcessResult)
/// UTF - 8 변환 유틸리티 함수
/// 주요기능 : 모든 모듈이 공유하는 기본 타입과 설정

#pragma once

// 크로스 플랫폼 소켓 정의 - 먼저 정의
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/select.h>
#include <climits>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <sstream>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <memory>
#include <queue>
#include <condition_variable>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <random>
#include <iomanip>
#include <deque>

// UTF-8 변환을 위한 헤더 - 경고 억제
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING 1
#define _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS 1
#define _CRT_SECURE_NO_WARNINGS 1

// 네트워크 상수
constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_CHUNK_SIZE = 1000;
constexpr int DEFAULT_CHUNK_COUNT = 100;

// 오류 코드
enum class ErrorCode {
    SUCCESS = 0,
    NETWORK_ERROR = 1,
    INVALID_DATA = 2,
    TIMEOUT = 3,
    CONFIGURATION_ERROR = 4
};

// UTF-8 변환 함수 - Windows API 사용으로 대체
#ifdef _WIN32
inline std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

inline std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}
#else
// Linux/macOS에서는 codecvt 사용 (경고 억제됨)
#include <codecvt>
#include <locale>

inline std::string wstring_to_utf8(const std::wstring& wstr) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(wstr);
}

inline std::wstring utf8_to_wstring(const std::string& str) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(str);
}
#endif

//==============================================================================
// Data Structures
//==============================================================================

struct Point3D {
    float x, y, z;

    Point3D(float x = 0.0f, float y = 0.0f, float z = 0.0f)
        : x(x), y(y), z(z) {
    }

    float distanceFromOrigin() const {
        return std::sqrt(x * x + y * y + z * z);
    }
};

struct PointCloudChunk {
    int chunk_id;
    std::vector<Point3D> points;

    PointCloudChunk() : chunk_id(0) {}

    explicit PointCloudChunk(int id) : chunk_id(id) {
        points.reserve(MAX_CHUNK_SIZE);
    }

    std::string serialize() const {
        std::ostringstream oss;
        oss << chunk_id << " " << points.size();
        for (const auto& point : points) {
            oss << " " << point.x << " " << point.y << " " << point.z;
        }
        return oss.str();
    }

    static PointCloudChunk deserialize(const std::string& data) {
        std::istringstream iss(data);
        PointCloudChunk chunk;
        size_t point_count;

        if (!(iss >> chunk.chunk_id >> point_count)) {
            throw std::runtime_error("Failed to parse chunk header");
        }

        chunk.points.reserve(point_count);
        for (size_t i = 0; i < point_count; ++i) {
            Point3D point;
            if (!(iss >> point.x >> point.y >> point.z)) {
                throw std::runtime_error("Failed to parse point data");
            }
            chunk.points.push_back(point);
        }
        return chunk;
    }

    bool empty() const { return points.empty(); }
    size_t size() const { return points.size(); }
};

struct ProcessResult {
    int chunk_id;
    float avg_distance;
    int point_count;

    ProcessResult() : chunk_id(0), avg_distance(0.0f), point_count(0) {}

    ProcessResult(int id, float avg_dist, int count)
        : chunk_id(id), avg_distance(avg_dist), point_count(count) {
    }

    std::string serialize() const {
        std::ostringstream oss;
        oss << chunk_id << " " << avg_distance << " " << point_count;
        return oss.str();
    }

    static ProcessResult deserialize(const std::string& data) {
        std::istringstream iss(data);
        ProcessResult result;
        if (!(iss >> result.chunk_id >> result.avg_distance >> result.point_count)) {
            throw std::runtime_error("Failed to parse process result");
        }
        return result;
    }
};

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

    bool isValid() const {
        return !ip_address.empty() && !username.empty() && !worker_path.empty();
    }
};

struct RuntimeSettings {
    // Master 설정
    int drain_seconds = 5;
    int master_starvation_seconds = 30;
    int emergency_local_spawn_max = 3;
    bool run_local_worker_on_master = true;

    // Worker 설정
    int worker_idle_timeout_seconds = 20;
    int worker_retry_max = 5;
    int worker_retry_backoff_ms = 200;
    int worker_recv_timeout_ms = 5000;
    int worker_send_timeout_ms = 5000;

    // SSH 설정
    int ssh_connect_timeout_sec = 5;
    int ssh_server_alive_interval_sec = 5;
    int ssh_server_alive_count_max = 2;
    bool ssh_batch_mode = true;
    std::string ssh_strict_host_key = "accept-new";
};