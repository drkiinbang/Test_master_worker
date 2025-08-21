#include "master.h"
#include "network.h"
#include <iostream>
#include <algorithm>
#include <random>

static std::string getSelfExePath() {
#ifdef _WIN32
    std::wstring wpath;
    DWORD cap = 260;
    for (;;) {
        std::vector<wchar_t> buf(cap);
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return std::string();
        }
        if (n < buf.size() - 1) {
            wpath.assign(buf.data(), n);
            break;
        }
        cap *= 2;
    }
    return wstring_to_utf8(wpath);
#elif __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::string();
    char resolved[PATH_MAX] = { 0 };
    if (realpath(buf.data(), resolved)) return std::string(resolved);
    return std::string(buf.data());
#else
    std::vector<char> buf(4096);
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    char resolved[PATH_MAX] = { 0 };
    if (realpath(buf.data(), resolved)) return std::string(resolved);
    return std::string(buf.data());
#endif
}

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

static int startLocalWorkerNewWindow(const std::string& exePath,
    const std::string& master_ip,
    int master_port) {
    const std::string base = "\"" + exePath + "\" worker " + master_ip + " " + std::to_string(master_port);

#if defined(_WIN32)
    std::string cmd = "cmd /c start \"\" " + base;
    return std::system(cmd.c_str());
#elif defined(__APPLE__)
    std::string bashLine = "bash -lc \\\"" + escapeDQ(base) + "; exec bash -i\\\"";
    std::string cmd = "osascript -e 'tell application \"Terminal\" to do script \"" + bashLine + "\"' "
        "-e 'tell application \"Terminal\" to activate'";
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
        "else "
        "tmux new-session -d -s pointcloud \"" + escapeDQ(base) + "\"; "
        "echo \"No desktop terminal found; started in tmux session 'pointcloud'\"; "
        "fi";
    std::string cmd = "bash -lc \"" + escapeDQ(inner) + "\"";
    return std::system(cmd.c_str());
#endif
}

RemoteWorkerManager::RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
    const std::string& master_ip, int master_port,
    const RuntimeSettings& settings)
    : remote_configs(configs), master_ip(master_ip),
    master_port(master_port), settings(settings) {
}

RemoteWorkerManager::~RemoteWorkerManager() {
    stop();
}

void RemoteWorkerManager::startRemoteWorkers() {
    for (const auto& cfg : remote_configs) {
        remote_threads.emplace_back([this, cfg]() { this->runRemoteWorker(cfg); });
    }
    std::wcout << L"Started " << remote_configs.size() << L" remote workers\n";
}

void RemoteWorkerManager::stop() {
    should_stop.store(true);
    for (auto& t : remote_threads) if (t.joinable()) t.join();
    remote_threads.clear();
}

void RemoteWorkerManager::runRemoteWorker(const RemoteWorkerConfig& config) {
    std::wcout << L"Starting remote worker on " << config.ip_address.c_str() << L"\n";
    std::stringstream ssh_command;
#ifdef _WIN32
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
        << " < NUL";
#else
    ssh_command << "ssh -p " << config.ssh_port
        << " -o ConnectTimeout=" << settings.ssh_connect_timeout_sec
        << " -o ServerAliveInterval=" << settings.ssh_server_alive_interval_sec
        << " -o ServerAliveCountMax=" << settings.ssh_server_alive_count_max
        << " -o ExitOnForwardFailure=yes"
        << " -o StrictHostKeyChecking=" << settings.ssh_strict_host_key
        << " -n";
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

MasterServer::MasterServer(int p, const std::string& config)
    : port(p), config_file(config) {
}

MasterServer::~MasterServer() {
    if (remote_manager) remote_manager->stop();
}

void MasterServer::generateSampleData() {
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

bool MasterServer::has_pending_work() const {
    return completed_chunk_ids_.size() < chunks.size();
}

void MasterServer::start() {
    ConfigManager::ensureConfig(config_file);
    settings = ConfigManager::loadRuntimeSettings(config_file);
    remote_configs = ConfigManager::loadRemoteWorkers(config_file);

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

    bool draining = false;
    auto drain_deadline = (std::chrono::steady_clock::time_point::max)();

    while (true) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(server_sock, &rfds);
        timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
#ifdef _WIN32
        int ready = select(0, &rfds, nullptr, nullptr, &tv);
#else
        int ready = select(server_sock + 1, &rfds, nullptr, nullptr, &tv);
#endif
        auto now = std::chrono::steady_clock::now();

        if (ready <= 0) {
            if (!draining && has_pending_work()) {
                auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_).count();
                if (idle >= settings.master_starvation_seconds) {
                    if (settings.run_local_worker_on_master &&
                        emergency_local_spawned_ < settings.emergency_local_spawn_max) {
                        const std::string selfExe = getSelfExePath();
                        int rc = startLocalWorkerNewWindow(selfExe, "127.0.0.1", this->port);
                        ++emergency_local_spawned_;
                        last_progress_ = now;
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
            std::string chunk_data = chunks[cid].serialize();
            bool sent_ok = NetworkUtils::sendData(client_sock, chunk_data);
            if (!sent_ok) {
                std::wcout << L"[WARN ] Failed to send chunk " << cid << L" to worker. Requeue.\n";
                requeue_chunk_if_needed(cid);
            }
            else {
                std::wcout << L"Sent chunk " << cid << L" to worker\n";
                std::string result_data = NetworkUtils::receiveData(client_sock);
                if (result_data.empty()) {
                    std::wcout << L"[WARN ] Worker disconnected before sending result for chunk "
                        << cid << L". Requeue.\n";
                    requeue_chunk_if_needed(cid);
                }
                else {
                    ProcessResult r = ProcessResult::deserialize(result_data);
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

void MasterServer::printFinalResults() {
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

bool MasterServer::try_acquire_next_chunk(int& out_cid) {
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

void MasterServer::requeue_chunk_if_needed(int cid) {
    std::lock_guard<std::mutex> lk(retry_mtx_);
    if (completed_chunk_ids_.find(cid) == completed_chunk_ids_.end()) {
        retry_queue_.push_back(cid);
    }
}