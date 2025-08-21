#include "worker.h"
#include "network.h"
#include <iostream>
#include <cmath>

WorkerClient::WorkerClient(const std::string& ip, int port, const RuntimeSettings& s)
    : master_ip(ip), master_port(port), settings(s) {
}

void WorkerClient::start() {
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
        fail_count = 0;
        last_success = clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool WorkerClient::connectAndProcess() {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) { std::wcerr << L"Socket creation failed\n"; return false; }

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

ProcessResult WorkerClient::processPointCloud(const PointCloudChunk& chunk) {
    std::wcout << L"Processing chunk " << chunk.chunk_id << L"...\n";
    float total_distance = 0.0f;
    for (const auto& p : chunk.points) {
        float d = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        total_distance += d;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

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