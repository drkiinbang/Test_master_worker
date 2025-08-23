//==============================================================================
// WorkerMonitor.h - 워커 상태 추적 및 관리
//==============================================================================

#pragma once

#include "Common.h"
#include "ProcessUtils.h"
#include "RemoteWorkerManager.h"

struct MonitorSettings {
    int heartbeat_interval_seconds = 10;
    int heartbeat_timeout_seconds = 30;
    int max_restart_attempts = 3;
    int restart_cooldown_seconds = 15;
    bool monitor_local_workers = true;
    bool monitor_remote_workers = true;
    bool auto_restart_failed_workers = true;
};

class WorkerMonitor {
public:
    WorkerMonitor(const MonitorSettings& settings);
    ~WorkerMonitor();

    void start();
    void stop();

    // 워커 등록/해제
    void registerWorker(const std::string& worker_id, const std::string& ip,
        int port, WorkerType type);
    void unregisterWorker(const std::string& worker_id);

    // 하트비트 처리
    void updateHeartbeat(const std::string& worker_id, const HeartbeatMessage& heartbeat);

    // 상태 조회
    std::vector<WorkerInfo> getWorkerStatus() const;
    size_t getActiveWorkerCount() const;

    // 재시작 콜백 설정
    void setRestartCallbacks(
        std::function<void(const std::string&)> restart_local_worker,
        std::function<void(const RemoteWorkerConfig&)> restart_remote_worker
    );

private:
    void monitorLoop();
    void checkWorkerHealth();
    void attemptWorkerRestart(const WorkerInfo& worker);

    MonitorSettings settings_;
    mutable std::mutex workers_mutex_;
    std::unordered_map<std::string, WorkerInfo> workers_;

    std::thread monitor_thread_;
    std::atomic<bool> should_stop_{ false };

    // 재시작 콜백
    std::function<void(const std::string&)> restart_local_callback_;
    std::function<void(const RemoteWorkerConfig&)> restart_remote_callback_;

    // 원격 워커 설정 (재시작용)
    std::unordered_map<std::string, RemoteWorkerConfig> remote_configs_;
};

//==============================================================================
// WorkerMonitor 구현부
//==============================================================================

WorkerMonitor::WorkerMonitor(const MonitorSettings& settings)
    : settings_(settings) {
}

WorkerMonitor::~WorkerMonitor() {
    stop();
}

void WorkerMonitor::start() {
    if (monitor_thread_.joinable()) {
        return;
    }

    should_stop_ = false;
    monitor_thread_ = std::thread(&WorkerMonitor::monitorLoop, this);
    std::wcout << L"Worker monitor started\n";
}

void WorkerMonitor::stop() {
    should_stop_ = true;
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    std::wcout << L"Worker monitor stopped\n";
}

void WorkerMonitor::registerWorker(const std::string& worker_id, const std::string& ip,
    int port, WorkerType type) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers_.emplace(worker_id, WorkerInfo(worker_id, ip, port, type));

    std::wcout << L"Registered worker: " << utf8_to_wstring(worker_id)
        << L" (" << utf8_to_wstring(ip) << L":" << port << L")\n";
}

void WorkerMonitor::unregisterWorker(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    auto it = workers_.find(worker_id);
    if (it != workers_.end()) {
        std::wcout << L"Unregistered worker: " << utf8_to_wstring(worker_id) << L"\n";
        workers_.erase(it);
    }
}

void WorkerMonitor::updateHeartbeat(const std::string& worker_id, const HeartbeatMessage& heartbeat) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    auto it = workers_.find(worker_id);
    if (it != workers_.end()) {
        it->second.last_heartbeat = std::chrono::steady_clock::now();
        it->second.status = heartbeat.status;
    }
}

std::vector<WorkerInfo> WorkerMonitor::getWorkerStatus() const {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    std::vector<WorkerInfo> status;
    status.reserve(workers_.size());

    for (const auto& pair : workers_) {
        status.push_back(pair.second);
    }

    return status;
}

size_t WorkerMonitor::getActiveWorkerCount() const {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    return std::count_if(workers_.begin(), workers_.end(),
        [](const auto& pair) {
            return pair.second.status == WorkerStatus::ACTIVE ||
                pair.second.status == WorkerStatus::IDLE;
        });
}

void WorkerMonitor::setRestartCallbacks(
    std::function<void(const std::string&)> restart_local_worker,
    std::function<void(const RemoteWorkerConfig&)> restart_remote_worker) {

    restart_local_callback_ = std::move(restart_local_worker);
    restart_remote_callback_ = std::move(restart_remote_worker);
}

void WorkerMonitor::monitorLoop() {
    while (!should_stop_) {
        checkWorkerHealth();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void WorkerMonitor::checkWorkerHealth() {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    auto now = std::chrono::steady_clock::now();

    for (auto& pair : workers_) {
        WorkerInfo& worker = pair.second;

        auto time_since_heartbeat = std::chrono::duration_cast<std::chrono::seconds>(
            now - worker.last_heartbeat).count();

        if (time_since_heartbeat > settings_.heartbeat_timeout_seconds) {
            if (worker.status != WorkerStatus::FAILED) {
                std::wcout << L"Worker " << utf8_to_wstring(worker.worker_id)
                    << L" heartbeat timeout (" << time_since_heartbeat << L"s)\n";
                worker.status = WorkerStatus::FAILED;

                // 재시작 시도
                if (settings_.auto_restart_failed_workers && worker.type != WorkerType::MANUAL) {
                    attemptWorkerRestart(worker);
                }
            }
        }
    }
}

void WorkerMonitor::attemptWorkerRestart(const WorkerInfo& worker) {
    if (worker.restart_count >= settings_.max_restart_attempts) {
        std::wcout << L"Worker " << utf8_to_wstring(worker.worker_id)
            << L" exceeded max restart attempts\n";
        return;
    }

    std::wcout << L"Attempting to restart worker: " << utf8_to_wstring(worker.worker_id)
        << L" (attempt " << (worker.restart_count + 1) << L")\n";

    if (worker.type == WorkerType::LOCAL_AUTO && restart_local_callback_) {
        restart_local_callback_(worker.worker_id);
    }
    else if (worker.type == WorkerType::REMOTE_AUTO && restart_remote_callback_) {
        auto config_it = remote_configs_.find(worker.worker_id);
        if (config_it != remote_configs_.end()) {
            restart_remote_callback_(config_it->second);
        }
    }

    // 재시작 횟수 증가는 실제 재시작 시도 후에 해야 함
    const_cast<WorkerInfo&>(worker).restart_count++;
}