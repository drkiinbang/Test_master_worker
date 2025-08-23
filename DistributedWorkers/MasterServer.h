/// 역할: 시스템의 중앙 제어 허브
/// TCP 서버 운영 및 클라이언트 연결 처리
/// 작업 분배 및 결과 수집
/// 워커 상태 모니터링
/// 드레이닝 페이즈 관리
/// 비상 워커 생성
/// 최종 결과 집계 및 출력
/// 주요기능 : runAcceptLoop() - 메인 서버 루프
#pragma once

#include "Common.h"
#include "Logger.h"
#include "NetworkUtils.h"
#include "ChunkProcessor.h"
#include "ConfigurationManager.h"
#include "ProcessUtils.h"
#include "RemoteWorkerManager.h"
#include "WorkerMonitor.h"

class MasterServer {
public:
    MasterServer(int port, const std::string& config_file = "master.config");
    ~MasterServer();

    void setChunks(std::vector<PointCloudChunk> chunks);
    ErrorCode start();
    void stop();

private:
    ErrorCode initializeServer();
    ErrorCode runAcceptLoop();
    void handleWorkerConnection(SOCKET client_socket, const std::string& client_ip);
    void handleWorkerMessage(SOCKET socket, const std::string& client_ip);
    void processHeartbeat(const std::string& data, const std::string& client_ip);

    void startLocalWorkerIfNeeded();
    void checkStarvationAndSpawnEmergencyWorker();
    void enterDrainingPhase();
    void printFinalResults() const;

    // 워커 재시작 콜백
    void restartLocalWorker(const std::string& worker_id);
    void restartRemoteWorker(const RemoteWorkerConfig& config);

    const int port_;
    const std::string config_file_;

    MasterSettings settings_;
    std::vector<RemoteWorkerConfig> remote_configs_;

    std::unique_ptr<WorkDistributor> work_distributor_;
    std::unique_ptr<RemoteWorkerManager> remote_manager_;
    std::unique_ptr<WorkerMonitor> worker_monitor_;  // 새로 추가

    mutable std::mutex results_mutex_;
    std::vector<ProcessResult> results_;

    std::atomic<bool> should_stop_{ false };
    std::atomic<bool> is_draining_{ false };
    std::chrono::steady_clock::time_point last_progress_;
    int emergency_spawned_count_{ 0 };

    SOCKET server_socket_{ INVALID_SOCKET };
};

//==============================================================================
// MasterServer 구현부
//==============================================================================

MasterServer::MasterServer(int port, const std::string& config_file)
    : port_(port), config_file_(config_file),
    last_progress_(std::chrono::steady_clock::now()) {
}

MasterServer::~MasterServer() {
    stop();
}

void MasterServer::setChunks(std::vector<PointCloudChunk> chunks) {
    work_distributor_ = std::make_unique<WorkDistributor>(std::move(chunks));
}

ErrorCode MasterServer::start() {
    if (!ConfigurationManager::ensureMasterConfigExists(config_file_)) {
        return ErrorCode::CONFIGURATION_ERROR;
    }

    settings_ = ConfigurationManager::loadMasterSettings(config_file_);
    remote_configs_ = ConfigurationManager::loadRemoteWorkers(config_file_);

    ErrorCode init_result = initializeServer();
    if (init_result != ErrorCode::SUCCESS) {
        return init_result;
    }

    // 워커 모니터링 시작
    MonitorSettings monitor_settings;
    monitor_settings.heartbeat_interval_seconds = settings_.heartbeat_interval_seconds;
    monitor_settings.heartbeat_timeout_seconds = settings_.heartbeat_timeout_seconds;
    monitor_settings.max_restart_attempts = settings_.max_restart_attempts;
    monitor_settings.restart_cooldown_seconds = settings_.restart_cooldown_seconds;
    monitor_settings.monitor_local_workers = settings_.monitor_local_workers;
    monitor_settings.monitor_remote_workers = settings_.monitor_remote_workers;
    monitor_settings.auto_restart_failed_workers = settings_.auto_restart_failed_workers;

    worker_monitor_ = std::make_unique<WorkerMonitor>(monitor_settings);
    worker_monitor_->setRestartCallbacks(
        [this](const std::string& worker_id) { restartLocalWorker(worker_id); },
        [this](const RemoteWorkerConfig& config) { restartRemoteWorker(config); }
    );
    worker_monitor_->start();

    if (settings_.run_local_worker_on_master) {
        startLocalWorkerIfNeeded();
    }

    if (!remote_configs_.empty()) {
        std::string master_ip = NetworkUtils::getLocalIPAddress();
        remote_manager_ = std::make_unique<RemoteWorkerManager>(
            remote_configs_, master_ip, port_, settings_);

        if (!remote_manager_->startRemoteWorkers()) {
            std::wcerr << L"Failed to start remote workers\n";
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    std::wcout << L"Master server listening on port " << port_ << L"\n";

    return runAcceptLoop();
}

void MasterServer::stop() {
    should_stop_ = true;

    if (worker_monitor_) {
        worker_monitor_->stop();
    }

    if (remote_manager_) {
        remote_manager_->stop();
    }

    if (server_socket_ != INVALID_SOCKET) {
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
    }
}

ErrorCode MasterServer::initializeServer() {
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ == INVALID_SOCKET) {
        Logger::error(L"Failed to create server socket");
        return ErrorCode::NETWORK_ERROR;
    }

    if (!NetworkUtils::setReuseAddress(server_socket_)) {
        Logger::warn(L"Failed to set SO_REUSEADDR");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (bind(server_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        Logger::error(L"Failed to bind server socket");
        closesocket(server_socket_);
        return ErrorCode::NETWORK_ERROR;
    }

    if (listen(server_socket_, 10) == SOCKET_ERROR) {
        Logger::error(L"Failed to listen on server socket");
        closesocket(server_socket_);
        return ErrorCode::NETWORK_ERROR;
    }

    return ErrorCode::SUCCESS;
}

ErrorCode MasterServer::runAcceptLoop() {
    auto drain_deadline = (std::chrono::steady_clock::time_point::max)();

    while (!should_stop_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket_, &read_fds);

        timeval timeout{ 1, 0 };

        int ready;
#ifdef _WIN32
        ready = select(0, &read_fds, nullptr, nullptr, &timeout);
#else
        ready = select(static_cast<int>(server_socket_) + 1, &read_fds, nullptr, nullptr, &timeout);
#endif

        auto now = std::chrono::steady_clock::now();

        if (ready <= 0) {
            if (!is_draining_ && work_distributor_->hasRemainingWork()) {
                checkStarvationAndSpawnEmergencyWorker();
            }

            if (is_draining_ && now >= drain_deadline) {
                std::wcout << L"Draining phase completed\n";
                break;
            }
            continue;
        }

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        SOCKET client_socket = accept(server_socket_,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len);

        if (client_socket == INVALID_SOCKET) continue;

        char ip_str[INET_ADDRSTRLEN] = { 0 };
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);

        handleWorkerConnection(client_socket, ip_str);

        if (!is_draining_ && !work_distributor_->hasRemainingWork()) {
            enterDrainingPhase();
            drain_deadline = now + std::chrono::seconds(settings_.drain_seconds);
        }
    }

    printFinalResults();
    return ErrorCode::SUCCESS;
}

void MasterServer::handleWorkerConnection(SOCKET client_socket, const std::string& client_ip) {
    RAIISocket socket(client_socket);

    try {
        MessageType msg_type;
        std::string msg_data;

        if (!NetworkUtils::receiveMessage(socket.get(), msg_type, msg_data)) {
            return;
        }

        switch (msg_type) {
        case MessageType::HEARTBEAT:
            processHeartbeat(msg_data, client_ip);
            break;

        case MessageType::CHUNK_DATA:
            // 기존 청크 처리 로직을 여기로 이동
            handleWorkerMessage(socket.release(), client_ip);
            break;

        default:
            std::wcout << L"Unknown message type from worker\n";
            break;
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Error handling worker connection: " << utf8_to_wstring(e.what()) << L"\n";
    }
}

void MasterServer::handleWorkerMessage(SOCKET client_socket, const std::string& client_ip) {
    std::wcout << L"Worker connected: " << utf8_to_wstring(client_ip) << L"\n";

    RAIISocket socket(client_socket);

    int chunk_id;
    bool has_work = !is_draining_ && work_distributor_->tryAcquireChunk(chunk_id);

    if (!has_work) {
        NetworkUtils::sendTerminationSignal(socket.get());
        std::wcout << L"Sent termination signal to worker\n";
        return;
    }

    try {
        const PointCloudChunk& chunk = work_distributor_->getChunk(chunk_id);
        std::string chunk_data = chunk.serialize();

        if (!NetworkUtils::sendMessage(socket.get(), MessageType::CHUNK_DATA, chunk_data)) {
            std::wcout << L"Failed to send chunk " << chunk_id << L"\n";
            work_distributor_->requeueChunk(chunk_id);
            return;
        }

        std::wcout << L"Sent chunk " << chunk_id << L" to worker\n";

        MessageType response_type;
        std::string result_data;
        if (!NetworkUtils::receiveMessage(socket.get(), response_type, result_data) ||
            response_type != MessageType::CHUNK_RESULT) {
            std::wcout << L"Worker disconnected before sending result\n";
            work_distributor_->requeueChunk(chunk_id);
            return;
        }

        ProcessResult result = ProcessResult::deserialize(result_data);

        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            work_distributor_->markCompleted(result.chunk_id);
            results_.push_back(result);

            std::wcout << L"Received result for chunk " << result.chunk_id
                << L": avg_distance=" << result.avg_distance
                << L", points=" << result.point_count << L"\n";
        }

        last_progress_ = std::chrono::steady_clock::now();

    }
    catch (const std::exception& e) {
        std::wcerr << L"Error handling worker: " << utf8_to_wstring(e.what()) << L"\n";
        work_distributor_->requeueChunk(chunk_id);
    }
}

void MasterServer::processHeartbeat(const std::string& data, const std::string& client_ip) {
    try {
        HeartbeatMessage heartbeat = HeartbeatMessage::deserialize(data);

        if (worker_monitor_) {
            worker_monitor_->updateHeartbeat(heartbeat.worker_id, heartbeat);
        }

        std::wcout << L"Heartbeat from " << utf8_to_wstring(heartbeat.worker_id)
            << L" (" << utf8_to_wstring(client_ip) << L") - "
            << L"Status: " << static_cast<int>(heartbeat.status)
            << L", Processed: " << heartbeat.processed_chunks << L"\n";
    }
    catch (const std::exception& e) {
        std::wcerr << L"Error processing heartbeat: " << utf8_to_wstring(e.what()) << L"\n";
    }
}

void MasterServer::startLocalWorkerIfNeeded() {
    std::string exe_path = ProcessUtils::getSelfExecutablePath();
    if (exe_path.empty()) {
        Logger::warn(L"Could not determine executable path for local worker");
        return;
    }

    ErrorCode result = ProcessUtils::startLocalWorkerNewWindow(exe_path, "127.0.0.1", port_);
    if (result == ErrorCode::SUCCESS) {
        Logger::info(L"Local worker started successfully");

        // 모니터링 시스템에 로컬 워커 등록
        if (worker_monitor_) {
            std::string worker_id = "local_worker_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            worker_monitor_->registerWorker(worker_id, "127.0.0.1", port_, WorkerType::LOCAL_AUTO);
        }
    }
    else {
        Logger::warn(L"Failed to start local worker");
    }
}

void MasterServer::restartLocalWorker(const std::string& worker_id) {
    std::wcout << L"Restarting local worker: " << utf8_to_wstring(worker_id) << L"\n";
    startLocalWorkerIfNeeded();
}

void MasterServer::restartRemoteWorker(const RemoteWorkerConfig& config) {
    std::wcout << L"Restarting remote worker: " << utf8_to_wstring(config.ip_address) << L"\n";

    // 원격 워커 매니저를 통해 특정 워커 재시작
    if (remote_manager_) {
        // RemoteWorkerManager에 개별 워커 재시작 기능이 필요함
        // 현재는 간단히 새로운 임시 매니저로 처리
        std::vector<RemoteWorkerConfig> single_config = { config };
        auto temp_manager = std::make_unique<RemoteWorkerManager>(
            single_config, NetworkUtils::getLocalIPAddress(), port_, settings_);
        temp_manager->startRemoteWorkers();
    }
}

void MasterServer::checkStarvationAndSpawnEmergencyWorker() {
    auto now = std::chrono::steady_clock::now();
    auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_progress_).count();

    if (idle_duration >= settings_.master_starvation_seconds &&
        emergency_spawned_count_ < settings_.emergency_local_spawn_max) {

        std::wcout << L"Starvation detected. Spawning emergency local worker\n";
        startLocalWorkerIfNeeded();
        emergency_spawned_count_++;
        last_progress_ = now;
    }
}

void MasterServer::enterDrainingPhase() {
    is_draining_ = true;
    std::wcout << L"All chunks processed! Entering draining phase for "
        << settings_.drain_seconds << L"s...\n";
}

void MasterServer::printFinalResults() const {
    std::lock_guard<std::mutex> lock(results_mutex_);

    std::wcout << L"\n=== Final Results ===\n";

    if (results_.empty()) {
        std::wcout << L"No results collected.\n";
        return;
    }

    float total_avg_distance = 0.0f;
    int total_points = 0;

    for (const auto& result : results_) {
        total_avg_distance += result.avg_distance;
        total_points += result.point_count;

        std::wcout << L"Chunk " << result.chunk_id
            << L": " << result.point_count << L" points, "
            << L"avg_distance=" << result.avg_distance << L"\n";
    }

    float overall_avg = total_avg_distance / static_cast<float>(results_.size());
    std::wcout << L"Overall average distance: " << overall_avg << L"\n";
    std::wcout << L"Total points processed: " << total_points << L"\n";
    std::wcout << L"Total chunks processed: " << results_.size() << L"\n";
}