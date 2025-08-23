/// 외부 설정 파일 관리
/// workers.conf 파일 파싱
/// 런타임 설정 로드(타임아웃, 재시도 등)
/// 원격 워커 정보 파싱
/// 기본 설정 파일 자동 생성
/// 주요기능 : 설정 기반 시스템 동작 제어

#pragma once

#include "Common.h"
#include "ProcessUtils.h"

// 마스터 전용 설정 구조체
struct MasterSettings {
    int drain_seconds = 5;
    int master_starvation_seconds = 30;
    int emergency_local_spawn_max = 3;
    bool run_local_worker_on_master = true;

    // SSH 설정
    int ssh_connect_timeout_sec = 5;
    int ssh_server_alive_interval_sec = 5;
    int ssh_server_alive_count_max = 2;
    bool ssh_batch_mode = true;
    std::string ssh_strict_host_key = "accept-new";

    // 워커 모니터링 설정 (새로 추가)
    int heartbeat_interval_seconds = 10;
    int heartbeat_timeout_seconds = 30;
    int max_restart_attempts = 3;
    int restart_cooldown_seconds = 15;
    bool monitor_local_workers = true;
    bool monitor_remote_workers = true;
    bool auto_restart_failed_workers = true;
};

// 워커 전용 설정 구조체
struct WorkerSettings {
    int worker_idle_timeout_seconds = 20;
    int worker_retry_max = 5;
    int worker_retry_backoff_ms = 200;
    int worker_recv_timeout_ms = 5000;
    int worker_send_timeout_ms = 5000;

    // 하트비트 설정 (새로 추가)
    int heartbeat_interval_seconds = 10;
    bool send_heartbeat = true;
};

class ConfigurationManager {
public:
    // 마스터 설정 관리
    static bool ensureMasterConfigExists(const std::string& config_file = "master.config");
    static MasterSettings loadMasterSettings(const std::string& config_file = "master.config");
    static std::vector<RemoteWorkerConfig> loadRemoteWorkers(const std::string& config_file = "master.config");

    // 워커 설정 관리
    static bool ensureWorkerConfigExists(const std::string& config_file = "worker.config");
    static WorkerSettings loadWorkerSettings(const std::string& config_file = "worker.config");

private:
    static void createDefaultMasterConfig(const std::string& config_file);
    static void createDefaultWorkerConfig(const std::string& config_file);
    static std::string getConfigPath(const std::string& config_file);
    static std::string trim(const std::string& str);
    static std::vector<std::string> split(const std::string& str, char delimiter);
    static bool parseBoolValue(const std::string& value);
};

//==============================================================================
// ConfigurationManager 구현부
//==============================================================================

std::string ConfigurationManager::getConfigPath(const std::string& config_file) {
    // 절대 경로인 경우 그대로 사용
    if (config_file.find(':') != std::string::npos || config_file[0] == '/') {
        return config_file;
    }

    // 상대 경로인 경우 실행파일 디렉토리 기준으로 변경
    std::string exe_path = ProcessUtils::getSelfExecutablePath();
    if (exe_path.empty()) {
        return config_file;
    }

    // 실행파일 경로에서 디렉토리 부분만 추출
    size_t last_slash = exe_path.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        std::string exe_dir = exe_path.substr(0, last_slash + 1);
        return exe_dir + config_file;
    }

    return config_file;
}

bool ConfigurationManager::ensureMasterConfigExists(const std::string& config_file) {
    std::string full_path = getConfigPath(config_file);
    std::ifstream file(full_path);
    if (!file.is_open()) {
        createDefaultMasterConfig(full_path);
        return true;
    }
    return true;
}

bool ConfigurationManager::ensureWorkerConfigExists(const std::string& config_file) {
    std::string full_path = getConfigPath(config_file);
    std::ifstream file(full_path);
    if (!file.is_open()) {
        createDefaultWorkerConfig(full_path);
        return true;
    }
    return true;
}

MasterSettings ConfigurationManager::loadMasterSettings(const std::string& config_file) {
    MasterSettings settings;
    std::string full_path = getConfigPath(config_file);
    std::ifstream file(full_path);

    if (!file.is_open()) {
        std::wcout << L"Using default master settings (config file not found)\n";
        return settings;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line.find('=') == std::string::npos) {
            continue;
        }

        size_t eq_pos = line.find('=');
        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));

        try {
            if (key == "drain_seconds") {
                settings.drain_seconds = std::stoi(value);
            }
            else if (key == "master_starvation_seconds") {
                settings.master_starvation_seconds = std::stoi(value);
            }
            else if (key == "emergency_local_spawn_max") {
                settings.emergency_local_spawn_max = std::stoi(value);
            }
            else if (key == "run_local_worker_on_master") {
                settings.run_local_worker_on_master = parseBoolValue(value);
            }
            // 새로 추가된 모니터링 설정들
            else if (key == "heartbeat_interval_seconds") {
                settings.heartbeat_interval_seconds = std::stoi(value);
            }
            else if (key == "heartbeat_timeout_seconds") {
                settings.heartbeat_timeout_seconds = std::stoi(value);
            }
            else if (key == "max_restart_attempts") {
                settings.max_restart_attempts = std::stoi(value);
            }
            else if (key == "restart_cooldown_seconds") {
                settings.restart_cooldown_seconds = std::stoi(value);
            }
            else if (key == "monitor_local_workers") {
                settings.monitor_local_workers = parseBoolValue(value);
            }
            else if (key == "monitor_remote_workers") {
                settings.monitor_remote_workers = parseBoolValue(value);
            }
            else if (key == "auto_restart_failed_workers") {
                settings.auto_restart_failed_workers = parseBoolValue(value);
            }
            // SSH 설정들
            else if (key == "ssh_connect_timeout_sec") {
                settings.ssh_connect_timeout_sec = std::stoi(value);
            }
            else if (key == "ssh_server_alive_interval_sec") {
                settings.ssh_server_alive_interval_sec = std::stoi(value);
            }
            else if (key == "ssh_server_alive_count_max") {
                settings.ssh_server_alive_count_max = std::stoi(value);
            }
            else if (key == "ssh_batch_mode") {
                settings.ssh_batch_mode = parseBoolValue(value);
            }
            else if (key == "ssh_strict_host_key") {
                settings.ssh_strict_host_key = value;
            }
        }
        catch (const std::exception& e) {
            std::wcerr << L"Failed to parse master config key '" << utf8_to_wstring(key)
                << L"': " << utf8_to_wstring(e.what()) << L"\n";
        }
    }

    return settings;
}

WorkerSettings ConfigurationManager::loadWorkerSettings(const std::string& config_file) {
    WorkerSettings settings;
    std::string full_path = getConfigPath(config_file);
    std::ifstream file(full_path);

    if (!file.is_open()) {
        std::wcout << L"Using default worker settings (config file not found)\n";
        return settings;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line.find('=') == std::string::npos) {
            continue;
        }

        size_t eq_pos = line.find('=');
        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));

        try {
            if (key == "worker_idle_timeout_seconds") {
                settings.worker_idle_timeout_seconds = std::stoi(value);
            }
            else if (key == "worker_retry_max") {
                settings.worker_retry_max = std::stoi(value);
            }
            else if (key == "worker_retry_backoff_ms") {
                settings.worker_retry_backoff_ms = std::stoi(value);
            }
            else if (key == "worker_recv_timeout_ms") {
                settings.worker_recv_timeout_ms = std::stoi(value);
            }
            else if (key == "worker_send_timeout_ms") {
                settings.worker_send_timeout_ms = std::stoi(value);
            }
            // 새로 추가된 하트비트 설정들
            else if (key == "heartbeat_interval_seconds") {
                settings.heartbeat_interval_seconds = std::stoi(value);
            }
            else if (key == "send_heartbeat") {
                settings.send_heartbeat = parseBoolValue(value);
            }
        }
        catch (const std::exception& e) {
            std::wcerr << L"Failed to parse worker config key '" << utf8_to_wstring(key)
                << L"': " << utf8_to_wstring(e.what()) << L"\n";
        }
    }

    return settings;
}

std::vector<RemoteWorkerConfig> ConfigurationManager::loadRemoteWorkers(const std::string& config_file) {
    std::vector<RemoteWorkerConfig> workers;
    std::string full_path = getConfigPath(config_file);
    std::ifstream file(full_path);

    if (!file.is_open()) {
        std::wcout << L"Master config file not found. Running with local workers only.\n";
        return workers;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line.find(',') == std::string::npos) {
            continue;
        }

        auto tokens = split(line, ',');
        if (tokens.size() >= 3) {
            RemoteWorkerConfig config;
            config.ip_address = trim(tokens[0]);
            config.username = trim(tokens[1]);
            config.worker_path = trim(tokens[2]);

            if (tokens.size() >= 4) {
                try {
                    config.ssh_port = std::stoi(trim(tokens[3]));
                }
                catch (...) {
                    config.ssh_port = 22;
                }
            }

            if (config.isValid()) {
                workers.push_back(config);
                std::wcout << L"Loaded remote worker: " << utf8_to_wstring(config.ip_address)
                    << L" (" << utf8_to_wstring(config.username) << L")\n";
            }
        }
    }

    return workers;
}

void ConfigurationManager::createDefaultMasterConfig(const std::string& config_file) {
    std::ofstream file(config_file);
    if (!file.is_open()) {
        std::wcerr << L"Failed to create master config file: " << utf8_to_wstring(config_file) << L"\n";
        return;
    }

    file << "# Master Server Configuration\n"
        << "# Point Cloud Distributed Processing System - Master Settings\n"
        << "# ==============================================================================\n\n"
        << "# ===== Master Server Settings =====\n\n"
        << "# drain_seconds: Wait time after all chunks processed (seconds)\n"
        << "drain_seconds=5\n\n"
        << "# master_starvation_seconds: Time before spawning emergency workers (seconds)\n"
        << "master_starvation_seconds=30\n\n"
        << "# emergency_local_spawn_max: Maximum emergency workers to spawn\n"
        << "emergency_local_spawn_max=3\n\n"
        << "# run_local_worker_on_master: Auto-start local worker on master startup\n"
        << "run_local_worker_on_master=true\n\n"
        << "# ===== Worker Monitoring Settings =====\n\n"
        << "# heartbeat_interval_seconds: Expected heartbeat interval from workers\n"
        << "heartbeat_interval_seconds=10\n\n"
        << "# heartbeat_timeout_seconds: Timeout before marking worker as failed\n"
        << "heartbeat_timeout_seconds=30\n\n"
        << "# max_restart_attempts: Maximum worker restart attempts\n"
        << "max_restart_attempts=3\n\n"
        << "# restart_cooldown_seconds: Cooldown between restart attempts\n"
        << "restart_cooldown_seconds=15\n\n"
        << "# monitor_local_workers: Monitor locally spawned workers\n"
        << "monitor_local_workers=true\n\n"
        << "# monitor_remote_workers: Monitor remote workers\n"
        << "monitor_remote_workers=true\n\n"
        << "# auto_restart_failed_workers: Automatically restart failed workers\n"
        << "auto_restart_failed_workers=true\n\n"
        << "# ===== SSH Remote Connection Settings =====\n\n"
        << "# ssh_connect_timeout_sec: SSH connection timeout (seconds)\n"
        << "ssh_connect_timeout_sec=5\n\n"
        << "# ssh_server_alive_interval_sec: SSH keepalive interval (seconds)\n"
        << "ssh_server_alive_interval_sec=5\n\n"
        << "# ssh_server_alive_count_max: Max SSH keepalive failures\n"
        << "ssh_server_alive_count_max=2\n\n"
        << "# ssh_batch_mode: Use key-based authentication only\n"
        << "ssh_batch_mode=true\n\n"
        << "# ssh_strict_host_key: Host key verification policy\n"
        << "ssh_strict_host_key=accept-new\n\n"
        << "# ===== Remote Workers Configuration =====\n"
        << "# Format: ip_address,username,worker_executable_path[,ssh_port]\n\n"
        << "# Examples (uncomment and modify for your environment):\n"
        << "#192.168.1.100,ubuntu,/home/ubuntu/DistributedPCProcess,22\n"
        << "#192.168.1.101,worker,/opt/pointcloud/worker,22\n"
        << "#192.168.1.200,Administrator,C:\\Workers\\DistributedPCProcess.exe,22\n";

    std::wcout << L"Created default master config: " << utf8_to_wstring(config_file) << L"\n";
}

void ConfigurationManager::createDefaultWorkerConfig(const std::string& config_file) {
    std::ofstream file(config_file);
    if (!file.is_open()) {
        std::wcerr << L"Failed to create worker config file: " << utf8_to_wstring(config_file) << L"\n";
        return;
    }

    file << "# Worker Configuration\n"
        << "# Point Cloud Distributed Processing System - Worker Settings\n"
        << "# ==============================================================================\n\n"
        << "# ===== Worker Settings =====\n\n"
        << "# worker_idle_timeout_seconds: Worker idle timeout (seconds)\n"
        << "worker_idle_timeout_seconds=20\n\n"
        << "# worker_retry_max: Maximum connection retry attempts\n"
        << "worker_retry_max=5\n\n"
        << "# worker_retry_backoff_ms: Retry backoff interval (milliseconds)\n"
        << "worker_retry_backoff_ms=200\n\n"
        << "# worker_recv_timeout_ms: Data receive timeout (milliseconds)\n"
        << "worker_recv_timeout_ms=5000\n\n"
        << "# worker_send_timeout_ms: Data send timeout (milliseconds)\n"
        << "worker_send_timeout_ms=5000\n\n"
        << "# ===== Heartbeat Settings =====\n\n"
        << "# heartbeat_interval_seconds: Heartbeat send interval (seconds)\n"
        << "heartbeat_interval_seconds=10\n\n"
        << "# send_heartbeat: Enable heartbeat sending to master\n"
        << "send_heartbeat=true\n\n"
        << "# ===== Performance Tuning Guide =====\n"
        << "# Fast network: Reduce timeout values (3000ms or less)\n"
        << "# Slow network: Increase timeout values (10000ms or more)\n"
        << "# Unstable connection: Increase retry_max and backoff_ms\n"
        << "# Debug mode: Increase all timeout values for easier debugging\n";

    std::wcout << L"Created default worker config: " << utf8_to_wstring(config_file) << L"\n";
}

std::string ConfigurationManager::trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";

    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::vector<std::string> ConfigurationManager::split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::istringstream iss(str);
    std::string token;

    while (std::getline(iss, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}

bool ConfigurationManager::parseBoolValue(const std::string& value) {
    std::string lower_value = value;
    std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(),
        [](unsigned char c) { return std::tolower(c); });

    return lower_value == "true" || lower_value == "yes" ||
        lower_value == "1" || lower_value == "on";
}