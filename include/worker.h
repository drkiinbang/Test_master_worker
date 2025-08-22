#pragma once
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>
#include <sstream>
#include <iostream>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define closesocket close
#endif

struct Point3D { float x,y,z; Point3D(float X=0,float Y=0,float Z=0):x(X),y(Y),z(Z){} };

struct PointCloudChunk {
    int chunk_id{}; std::vector<Point3D> points;
    static PointCloudChunk deserialize(const std::string& data) {
        PointCloudChunk c; std::stringstream ss(data); size_t n=0; ss >> c.chunk_id >> n;
        c.points.reserve(n);
        for (size_t i=0;i<n;++i){ Point3D p; ss>>p.x>>p.y>>p.z; c.points.push_back(p); }
        return c;
    }
};

struct ProcessResult {
    int chunk_id{}; float avg_distance{}; int point_count{};
    std::string serialize() const { std::stringstream ss; ss<<chunk_id<<" "<<avg_distance<<" "<<point_count; return ss.str(); }
};

class NetworkUtilsW {
public:
    static bool sendData(SOCKET s, const std::string& data) {
        uint32_t size = static_cast<uint32_t>(data.size());
        if (send(s, reinterpret_cast<const char*>(&size), sizeof(size), 0) != sizeof(size)) return false;
        const char* buf = data.data();
        int total=0, need=(int)size;
        while (total<need) {
            int sent = send(s, buf+total, need-total, 0);
            if (sent == SOCKET_ERROR) return false;
            total += sent;
        }
        return true;
    }
    static std::string receiveData(SOCKET s) {
        uint32_t size=0;
        int recvd = recv(s, reinterpret_cast<char*>(&size), sizeof(size), 0);
        if (recvd != sizeof(size)) return "";
        std::string data(size, '\0');
        int total=0, need=(int)size;
        while (total<need) {
            int got = recv(s, &data[total], need-total, 0);
            if (got == SOCKET_ERROR || got==0) return "";
            total += got;
        }
        return data;
    }
};

inline void run_worker(const std::string& master_ip, int master_port, const std::string& /*worker_conf*/) {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
#endif
    std::cout << "[WORKER] Connecting to " << master_ip << ":" << master_port << "\n";

    using clock = std::chrono::steady_clock;
    auto last_success = clock::now();
    int fail_count = 0;

    const int worker_idle_timeout_seconds = 20;
    const int worker_retry_max = 5;
    const int worker_retry_backoff_ms = 200;
    const int recv_timeout_ms = 5000;
    const int send_timeout_ms = 5000;

    while (true) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) { std::cerr << "[WORKER] socket failed\n"; break; }

#ifdef _WIN32
        DWORD rcv=recv_timeout_ms, snd=send_timeout_ms;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof(snd));
#else
        auto to_tv = [](int ms){ timeval tv{}; tv.tv_sec = ms/1000; tv.tv_usec=(ms%1000)*1000; return tv; };
        timeval rtv = to_tv(recv_timeout_ms);
        timeval stv = to_tv(send_timeout_ms);
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
#endif
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(master_port);
        if (inet_pton(AF_INET, master_ip.c_str(), &addr.sin_addr) != 1) {
            std::cerr << "[WORKER] invalid master ip\n"; closesocket(sock); break;
        }
        if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "[WORKER] connect failed\n"; closesocket(sock);
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - last_success).count();
            if (idle >= worker_idle_timeout_seconds || ++fail_count >= worker_retry_max) {
                std::cout << "[WORKER] No response. Exit.\n"; break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(worker_retry_backoff_ms * fail_count));
            continue;
        }

        std::string msg = NetworkUtilsW::receiveData(sock);
        if (msg.empty()) {
            closesocket(sock);
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - last_success).count();
            if (idle >= worker_idle_timeout_seconds || ++fail_count >= worker_retry_max) {
                std::cout << "[WORKER] No work/response. Exit.\n"; break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(worker_retry_backoff_ms * fail_count));
            continue;
        }
        if (msg == "TERMINATE") {
            std::cout << "[WORKER] Terminated by master.\n";
            closesocket(sock); break;
        }

        PointCloudChunk c = PointCloudChunk::deserialize(msg);
        float total = 0.f;
        for (const auto& p : c.points) total += std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        ProcessResult r;
        r.chunk_id = c.chunk_id;
        r.point_count = (int)c.points.size();
        r.avg_distance = (r.point_count>0) ? (total / r.point_count) : 0.f;

        NetworkUtilsW::sendData(sock, r.serialize());
        closesocket(sock);

        last_success = clock::now();
        fail_count = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#ifdef _WIN32
    WSACleanup();
#endif
}
