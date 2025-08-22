#pragma once

#include <sstream>
#include <string>

/// 처리 결과 구조체
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

#include "PointData.h"
#include "Worker.h"

#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

/// 원격 워커 실행 관리자
/// - ssh 명령으로 원격 머신에서 워커 프로세스를 실행
/// - 실패 시 재시도하며, stop() 호출 시 안전하게 스레드를 정리
/// 수정 가이드:
///  - plink/pscp 등 다른 SSH 클라이언트 사용 시 명령어 생성 부분 교체
///  - key 기반 인증을 전제로 하므로, ssh-copy-id 등으로 미리 키를 배포
class RemoteWorkerManager {
private:
    std::vector<RemoteWorkerConfig> remote_configs;
    std::vector<std::thread> remote_threads;
    std::string master_ip;
    int master_port;
    RuntimeSettings settings;
    std::atomic<bool> should_stop{ false };

public:
    RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
        const std::string& master_ip, int master_port,
        const RuntimeSettings& settings)
        : remote_configs(configs), master_ip(master_ip),
        master_port(master_port), settings(settings) {
    }

    ~RemoteWorkerManager() { stop(); }

    void startRemoteWorkers() {
        for (const auto& cfg : remote_configs) {
            remote_threads.emplace_back([this, cfg]() { this->runRemoteWorker(cfg); });
        }
        std::wcout << L"Started " << remote_configs.size() << L" remote workers\n";
    }

    void stop() {
        should_stop.store(true);
        for (auto& t : remote_threads) if (t.joinable()) t.join();
        remote_threads.clear();
    }

private:
    void runRemoteWorker(const RemoteWorkerConfig& config) {

        std::wcout << L"Starting remote worker on " << config.ip_address.c_str() << L"\n";
        std::stringstream ssh_command;
#ifdef _WIN32
        /// Windows: OpenSSH 클라이언트(ssh)가 PATH에 있어야 합니다.
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
            << " < NUL"; /// detach stdin
#else
        ssh_command << "ssh -p " << config.ssh_port
            << " -o ConnectTimeout=" << settings.ssh_connect_timeout_sec
            << " -o ServerAliveInterval=" << settings.ssh_server_alive_interval_sec
            << " -o ServerAliveCountMax=" << settings.ssh_server_alive_count_max
            << " -o ExitOnForwardFailure=yes"
            << " -o StrictHostKeyChecking=" << settings.ssh_strict_host_key
            << " -n"; /// detach stdin
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
};

/// ---------- Master ----------
class MasterServer {
private:
    std::vector<PointCloudChunk> chunks; /// 전송할 chunk
    std::vector<ProcessResult>   results; /// 수집된 결과
    std::atomic<int>             chunk_index{ 0 };
    std::mutex                   results_mutex;
    int                          port;
    std::atomic<bool>            all_chunks_processed{ false };
    std::unique_ptr<RemoteWorkerManager> remote_manager;
    std::string                  config_file;
    RuntimeSettings              settings;
    std::vector<RemoteWorkerConfig> remote_configs;
    /// 재배당 대기열 및 완료 집합
    std::deque<int> retry_queue_;
    std::mutex      retry_mtx_;
    std::unordered_set<int> completed_chunk_ids_;
    std::chrono::steady_clock::time_point last_progress_{ std::chrono::steady_clock::now() };
    int emergency_local_spawned_ = 0;

public:
    MasterServer(int p, const std::string& config = "workers.conf")
        : port(p), config_file(config) {
    }

    ~MasterServer() {
        if (remote_manager) remote_manager->stop();
    }

    /// 테스트를 위한 샘플 생성
    void setSampleData(const std::vector<PointCloudChunk>& sampleData) {
        this->chunks = sampleData;
    }

    bool has_pending_work() const {
        /// 완료한 고유 청크 수 < 전체 청크 수 면 아직 할 일 있음
        return completed_chunk_ids_.size() < chunks.size();
    }

    /// 서버 시작:
    /// 1) (옵션) 원격 워커 자동 실행
    /// 2) TCP listen → accept 루프에서 워커에 청크 전송/결과 수신
    void start() {
        /// Load config (create default if missing)
        ConfigManager::ensureConfig(config_file);
        settings = ConfigManager::loadRuntimeSettings(config_file);
        remote_configs = ConfigManager::loadRemoteWorkers(config_file);

        /// Start a local worker
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

        /// Create listening socket
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

        /// Start remote workers AFTER listening is ready
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

        /// Draining-aware accept loop
        bool draining = false;
        auto drain_deadline = (std::chrono::steady_clock::time_point::max)();

        while (true) {
            /// Select with 1s timeout to check draining deadline
            fd_set rfds; FD_ZERO(&rfds); FD_SET(server_sock, &rfds);
            timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
#ifdef _WIN32
            int ready = select(0, &rfds, nullptr, nullptr, &tv); /// nfds ignored on Windows
#else
            int ready = select(server_sock + 1, &rfds, nullptr, nullptr, &tv);
#endif
            auto now = std::chrono::steady_clock::now();

            if (ready <= 0) {
                // ★ 굶주림 워치독: 접속이 "없을 때"도 체크해야 재기동이 됨
                if (!draining && has_pending_work()) {
                    auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_).count();
                    if (idle >= settings.master_starvation_seconds) {
                        if (settings.run_local_worker_on_master &&
                            emergency_local_spawned_ < settings.emergency_local_spawn_max) {

                            const std::string selfExe = getSelfExePath();
                            int rc = startLocalWorkerNewWindow(selfExe, "127.0.0.1", this->port);
                            ++emergency_local_spawned_;
                            last_progress_ = now;
                            // (로그는 기존 코드 그대로)
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
                // 청크 전송
                std::string chunk_data = chunks[cid].serialize();
                bool sent_ok = NetworkUtils::sendData(client_sock, chunk_data);
                if (!sent_ok) {
                    std::wcout << L"[WARN ] Failed to send chunk " << cid << L" to worker. Requeue.\n";
                    requeue_chunk_if_needed(cid);
                }
                else {
                    std::wcout << L"Sent chunk " << cid << L" to worker\n";
                    // 결과 수신
                    std::string result_data = NetworkUtils::receiveData(client_sock);
                    if (result_data.empty()) {
                        std::wcout << L"[WARN ] Worker disconnected before sending result for chunk "
                            << cid << L". Requeue.\n";
                        requeue_chunk_if_needed(cid);
                    }
                    else {
                        ProcessResult r = ProcessResult::deserialize(result_data);
                        // 중복 결과 방지(느리게 도착한 중복 결과/재배정 후 중복 대비)
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

                            // 모든 청크 완료 → 드레이닝 시작
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
                /// 더 줄 일이 없으면 종료 신호
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

private:
    /// 최종 결과 출력
    void printFinalResults() {
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

    /// 재배당 대기열 우선 → 없으면 전진 인덱스에서 하나 할당
    bool try_acquire_next_chunk(int& out_cid) {
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

    /// 실패/중단 시 청크 재배치
    void requeue_chunk_if_needed(int cid) {
        std::lock_guard<std::mutex> lk(retry_mtx_);
        // 이미 결과를 받은 청크는 재배정하지 않음(중복 방지)
        if (completed_chunk_ids_.find(cid) == completed_chunk_ids_.end()) {
            retry_queue_.push_back(cid);
        }
    }
};