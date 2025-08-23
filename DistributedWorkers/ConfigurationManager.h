/// 외부 설정 파일 관리
/// workers.conf 파일 파싱
/// 런타임 설정 로드(타임아웃, 재시도 등)
/// 원격 워커 정보 파싱
/// 기본 설정 파일 자동 생성
/// 주요기능 : 설정 기반 시스템 동작 제어
#pragma once

#include "Common.h"
#include "ProcessUtils.h"

class ConfigurationManager {
public:
    static bool ensureConfigExists(const std::string& config_file);
    static RuntimeSettings loadRuntimeSettings(const std::string& config_file);
    static std::vector<RemoteWorkerConfig> loadRemoteWorkers(const std::string& config_file);
    static std::string getConfigPath(const std::string& config_file);

private:
    static void createDefaultConfig(const std::string& config_file);
    static std::string trim(const std::string& str);
    static std::vector<std::string> split(const std::string& str, char delimiter);
    static bool parseBoolValue(const std::string& value);
};

//==============================================================================
// ConfigurationManager 구현부
//==============================================================================

bool ConfigurationManager::ensureConfigExists(const std::string& config_file) {
    std::string full_path = getConfigPath(config_file);
    std::ifstream file(full_path);
    if (!file.is_open()) {
        createDefaultConfig(full_path);
        return true;
    }
    return true;
}

RuntimeSettings ConfigurationManager::loadRuntimeSettings(const std::string& config_file) {
    RuntimeSettings settings;
    std::string full_path = getConfigPath(config_file);
    std::ifstream file(full_path);

    if (!file.is_open()) {
        std::wcout << L"Using default settings (config file not found)\n";
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
            else if (key == "worker_idle_timeout_seconds") {
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
            std::wcerr << L"Failed to parse config key '" << utf8_to_wstring(key)
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
        std::wcout << L"Config file not found. Running with local workers only.\n";
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

std::string ConfigurationManager::getConfigPath(const std::string& config_file) {
    // 절대 경로인 경우 그대로 사용
    if (config_file.find(':') != std::string::npos || config_file[0] == '/') {
        return config_file;
    }

    // 상대 경로인 경우 실행파일 디렉토리 기준으로 변경
    std::string exe_path = ProcessUtils::getSelfExecutablePath();
    if (exe_path.empty()) {
        return config_file;  // 실패시 원래 경로 사용
    }

    // 실행파일 경로에서 디렉토리 부분만 추출
    size_t last_slash = exe_path.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        std::string exe_dir = exe_path.substr(0, last_slash + 1);
        return exe_dir + config_file;
    }

    return config_file;
}

void ConfigurationManager::createDefaultConfig(const std::string& config_file) {
    std::string full_path = getConfigPath(config_file);
    std::ofstream file(full_path);
    if (!file.is_open()) {
        std::wcerr << L"Failed to create config file: " << utf8_to_wstring(full_path) << L"\n";
        return;
    }

    file << "# Point Cloud Distributed Processing System Configuration\n"
        << "# Lines with commas are remote workers: ip,username,worker_path,ssh_port\n"
        << "# Lines with key=value are runtime settings\n\n"
        << "# ===== Master Settings =====\n"
        << "drain_seconds=5\n"
        << "master_starvation_seconds=30\n"
        << "emergency_local_spawn_max=3\n"
        << "run_local_worker_on_master=true\n\n"
        << "# ===== Worker Settings =====\n"
        << "worker_idle_timeout_seconds=20\n"
        << "worker_retry_max=5\n"
        << "worker_retry_backoff_ms=200\n"
        << "worker_recv_timeout_ms=5000\n"
        << "worker_send_timeout_ms=5000\n\n"
        << "# ===== SSH Settings =====\n"
        << "ssh_connect_timeout_sec=5\n"
        << "ssh_server_alive_interval_sec=5\n"
        << "ssh_server_alive_count_max=2\n"
        << "ssh_batch_mode=yes\n"
        << "ssh_strict_host_key=accept-new\n\n"
        << "# ===== Remote Workers (Examples) =====\n"
        << "# Format: ip,username,worker_path[,ssh_port]\n"
        << "#192.168.1.100,user,/path/to/worker,22\n"
        << "#192.168.1.101,user,C:\\Workers\\worker.exe,22\n";
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