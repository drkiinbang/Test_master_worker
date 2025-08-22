#pragma once
#include <string>
#include <vector>
#include <deque>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <sstream>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <cstdlib>

#include "worker_config.h"
#include "ssh_exec.h"

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

struct Point3D { float x, y, z; Point3D(float X=0, float Y=0, float Z=0):x(X),y(Y),z(Z){} };

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
        PointCloudChunk c; std::stringstream ss(data); size_t n=0; ss >> c.chunk_id >> n;
        c.points.reserve(n);
        for (size_t i=0;i<n;++i){ Point3D p; ss>>p.x>>p.y>>p.z; c.points.push_back(p); }
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

struct RuntimeSettings {
    int drain_seconds = 5;
    int master_starvation_seconds = 30;
    int emergency_local_spawn_max = 1;
};

class MasterServer {
public:
    MasterServer(const std::string& ip, int port, const std::vector<RemoteWorkerConfig>& remotes,
                 const RuntimeSettings& s, const std::string& selfExe)
        : ip_(ip), port_(port), remote_configs_(remotes), settings_(s), self_exe_(selfExe) {}

    void generateSampleData() {
        std::mt19937 gen{std::random_device{}()};
        std::uniform_real_distribution<float> dis(-100.f, 100.f);
        for (int cid=0; cid<10; ++cid) {
            PointCloudChunk c; c.chunk_id = cid;
            for (int i=0; i<1000; ++i) c.points.emplace_back(dis(gen), dis(gen), dis(gen));
            chunks_.push_back(std::move(c));
        }
        std::cout << "Generated " << chunks_.size() << " chunks with total "
                  << (chunks_.size()*1000) << " points\n";
    }

    void start() {
#ifdef _WIN32
        WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
#endif
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock == INVALID_SOCKET) { std::cerr << "[MASTER] socket failed\n"; return; }
        int opt = 1; setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons(port_);
        if (bind(server_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "[MASTER] bind failed\n"; closesocket(server_sock); return;
        }
        if (listen(server_sock, 16) == SOCKET_ERROR) {
            std::cerr << "[MASTER] listen failed\n"; closesocket(server_sock); return;
        }
        std::cout << "Master server listening on port " << port_ << "\n";

        if (!remote_configs_.empty()) {
            for (const auto& w : remote_configs_) {
                std::string cmd = build_remote_exec(w, ip_, port_, (std::filesystem::current_path()/ "workers.conf").string());
                std::cout << "Starting remote worker on " << w.host << " ...\n";
                std::thread([cmd]{
                    while (true) {
                        int rc = std::system(cmd.c_str());
                        (void)rc;
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                    }
                }).detach();
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        } else {
            std::cout << "No remote workers configured.\n";
        }

        bool draining = false;
        auto drain_deadline = std::chrono::steady_clock::time_point::max();

        while (true) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(server_sock, &rfds);
            timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
#ifdef _WIN32
            int ready = select(0, &rfds, nullptr, nullptr, &tv);
#else
            int ready = select(server_sock+1, &rfds, nullptr, nullptr, &tv);
#endif
            if (ready <= 0) {
                if (draining && std::chrono::steady_clock::now() >= drain_deadline) break;
                continue;
            }

            sockaddr_in caddr{}; socklen_t clen = sizeof(caddr);
            SOCKET cs = accept(server_sock, (sockaddr*)&caddr, &clen);
            if (cs == INVALID_SOCKET) continue;

            int cid = INT32_MAX;
            bool has_work = (!draining) && try_acquire_next_chunk(cid);

            if (has_work) {
                std::string payload = chunks_[cid].serialize();
                if (!NetworkUtils::sendData(cs, payload)) {
                    requeue_chunk_if_needed(cid);
                    closesocket(cs);
                    continue;
                }
                std::string result = NetworkUtils::receiveData(cs);
                if (result.empty()) {
                    requeue_chunk_if_needed(cid);
                } else {
                    ProcessResult r = ProcessResult::deserialize(result);
                    {
                        std::lock_guard<std::mutex> lk(results_mtx_);
                        if (completed_.insert(r.chunk_id).second) {
                            results_.push_back(r);
                            if (completed_.size() == chunks_.size() && !draining) {
                                draining = true;
                                drain_deadline = std::chrono::steady_clock::now()
                                               + std::chrono::seconds(settings_.drain_seconds);
                                std::cout << "All chunks processed. Draining for "
                                          << settings_.drain_seconds << "s\n";
                            }
                        }
                    }
                }
            } else {
                NetworkUtils::sendTerminationSignal(cs);
            }
            closesocket(cs);
        }

        closesocket(server_sock);
        printFinalResults();
#ifdef _WIN32
        WSACleanup();
#endif
    }

private:
    bool try_acquire_next_chunk(int& out_cid) {
        std::lock_guard<std::mutex> lk(retry_mtx_);
        if (!retry_q_.empty()) {
            out_cid = retry_q_.front(); retry_q_.pop_front(); return true;
        }
        int idx = chunk_index_.fetch_add(1);
        if (idx < (int)chunks_.size()) { out_cid = idx; return true; }
        return false;
    }
    void requeue_chunk_if_needed(int cid) {
        std::lock_guard<std::mutex> lk(retry_mtx_);
        if (!completed_.count(cid)) retry_q_.push_back(cid);
    }
    void printFinalResults() {
        std::cout << "\n=== Final Results ===\n";
        float tot = 0.f; int points=0;
        for (auto& r : results_) { tot += r.avg_distance; points += r.point_count;
            std::cout << "Chunk " << r.chunk_id << ": " << r.point_count
                      << " pts, avg=" << r.avg_distance << "\n";
        }
        if (!results_.empty())
            std::cout << "Overall avg: " << (tot / results_.size())
                      << " | Total points: " << points << "\n";
    }

private:
    std::string ip_;
    int port_{};
    std::vector<PointCloudChunk> chunks_;
    std::vector<ProcessResult>   results_;
    std::atomic<int>             chunk_index_{0};
    std::mutex                   results_mtx_;
    std::unordered_set<int>      completed_;
    std::deque<int>              retry_q_;
    std::mutex                   retry_mtx_;
    std::vector<RemoteWorkerConfig> remote_configs_;
    RuntimeSettings settings_;
    std::string self_exe_;
};

inline void run_master(const std::string& ip, int port) {
    auto exe_dir = std::filesystem::current_path().string();
    auto workers = load_remote_workers(exe_dir);
    RuntimeSettings s;
#ifdef _WIN32
    char buf[MAX_PATH]{0}; GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string selfExe = buf;
#else
    std::string selfExe = "./Test_master_worker";
#endif
    MasterServer server(ip, port, workers, s, selfExe);
    server.generateSampleData();
    server.start();
}
