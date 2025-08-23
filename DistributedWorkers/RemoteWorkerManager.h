/// 원격 워커 배포 및 관리
/// SSH 명령어 생성 및 실행
/// 원격 워커 스레드 관리
/// 원격 연결 실패 처리
/// 워커 라이프사이클 관리
/// 주요기능 : SSH를 통한 자동 원격 배포

#pragma once

#include "Common.h"

class RemoteWorkerManager {
public:
    RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
        const std::string& master_ip, int master_port,
        const MasterSettings& settings);

    ~RemoteWorkerManager();

    bool startRemoteWorkers();
    void stop();

private:
    void runRemoteWorker(const RemoteWorkerConfig& config);
    std::string buildSSHCommand(const RemoteWorkerConfig& config) const;

    const std::vector<RemoteWorkerConfig> remote_configs_;
    const std::string master_ip_;
    const int master_port_;
    const MasterSettings settings_;

    std::vector<std::thread> worker_threads_;
    std::atomic<bool> should_stop_{ false };
};

//==============================================================================
// RemoteWorkerManager 구현부
//==============================================================================

RemoteWorkerManager::RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
    const std::string& master_ip, int master_port,
    const MasterSettings& settings)
    : remote_configs_(configs), master_ip_(master_ip),
    master_port_(master_port), settings_(settings) {
}

RemoteWorkerManager::~RemoteWorkerManager() {
    stop();
}

bool RemoteWorkerManager::startRemoteWorkers() {
    if (remote_configs_.empty()) {
        std::wcout << L"No remote workers configured\n";
        return true;
    }

    std::wcout << L"Starting " << remote_configs_.size() << L" remote workers\n";

    worker_threads_.reserve(remote_configs_.size());

    for (const auto& config : remote_configs_) {
        if (!config.isValid()) {
            std::wcerr << L"Invalid remote worker config: "
                << utf8_to_wstring(config.ip_address) << L"\n";
            continue;
        }

        worker_threads_.emplace_back([this, config]() {
            runRemoteWorker(config);
            });
    }

    return !worker_threads_.empty();
}

void RemoteWorkerManager::stop() {
    should_stop_.store(true);

    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    worker_threads_.clear();
}

void RemoteWorkerManager::runRemoteWorker(const RemoteWorkerConfig& config) {
    std::wcout << L"Starting remote worker on " << utf8_to_wstring(config.ip_address) << L"\n";

    while (!should_stop_.load()) {
        std::string ssh_command = buildSSHCommand(config);
        std::wcout << L"Executing SSH command for " << utf8_to_wstring(config.ip_address) << L"\n";

        int return_code = std::system(ssh_command.c_str());

        if (return_code == 0) {
            std::wcout << L"Remote worker on " << utf8_to_wstring(config.ip_address)
                << L" completed successfully\n";
        }
        else {
            std::wcout << L"Remote worker on " << utf8_to_wstring(config.ip_address)
                << L" failed (code: " << return_code << L")\n";
        }

        if (!should_stop_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    std::wcout << L"Remote worker thread for " << utf8_to_wstring(config.ip_address)
        << L" terminated\n";
}

std::string RemoteWorkerManager::buildSSHCommand(const RemoteWorkerConfig& config) const {
    // 입력 검증
    if (config.ip_address.find_first_of("\"'`$;|&") != std::string::npos ||
        config.username.find_first_of("\"'`$;|&") != std::string::npos ||
        config.worker_path.find_first_of("\"'`$;|&") != std::string::npos) {
        throw std::invalid_argument("Invalid characters in SSH configuration");
    }

    std::ostringstream ssh_cmd;

#ifdef _WIN32
    ssh_cmd << "ssh -p " << config.ssh_port
        << " -o ConnectTimeout=" << settings_.ssh_connect_timeout_sec
        << " -o ServerAliveInterval=" << settings_.ssh_server_alive_interval_sec
        << " -o ServerAliveCountMax=" << settings_.ssh_server_alive_count_max
        << " -o StrictHostKeyChecking=" << settings_.ssh_strict_host_key;

    if (settings_.ssh_batch_mode) {
        ssh_cmd << " -o BatchMode=yes";
    }

    // 명령어 이스케이핑 강화
    ssh_cmd << " \"" << config.username << "\"@\"" << config.ip_address << "\""
        << " \"\\\"" << config.worker_path << "\\\""
        << " worker " << master_ip_ << " " << master_port_
        << " worker.config\""
        << " < NUL";
#else
    ssh_cmd << "ssh -p " << config.ssh_port
        << " -o ConnectTimeout=" << settings_.ssh_connect_timeout_sec
        << " -o ServerAliveInterval=" << settings_.ssh_server_alive_interval_sec
        << " -o ServerAliveCountMax=" << settings_.ssh_server_alive_count_max
        << " -o ExitOnForwardFailure=yes"
        << " -o StrictHostKeyChecking=" << settings_.ssh_strict_host_key
        << " -n";

    if (settings_.ssh_batch_mode) {
        ssh_cmd << " -o BatchMode=yes";
    }

    // 쉘 인젝션 방지를 위한 이스케이핑
    ssh_cmd << " '" << config.username << "'@'" << config.ip_address << "'"
        << " '" << config.worker_path
        << " worker " << master_ip_ << " " << master_port_
        << " worker.config'";
#endif

    return ssh_cmd.str();
}