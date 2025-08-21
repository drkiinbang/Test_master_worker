#include "config.h"
#include <fstream>
#include <iostream>
#include <algorithm>

void ConfigManager::createDefaultConfig(const std::string& config_file) {
    std::ofstream file(config_file);
    if (!file.is_open()) return;

    file << "# Remote Worker Configuration File\n";
    file << "# Lines with commas are remote workers: ip,username,worker_path,ssh_port\n";
    file << "# Lines with key=value are settings.\n\n";

    file << "# ===== Settings (defaults) =====\n";
    file << "drain_seconds=5\n";
    file << "worker_idle_timeout_seconds=20\n";
    file << "worker_retry_max=5\n";
    file << "worker_retry_backoff_ms=200\n";
    file << "worker_recv_timeout_ms=5000\n";
    file << "worker_send_timeout_ms=5000\n";
    file << "ssh_connect_timeout_sec=5\n";
    file << "ssh_server_alive_interval_sec=5\n";
    file << "ssh_server_alive_count_max=2\n";
    file << "ssh_batch_mode=yes\n";
    file << "ssh_strict_host_key=accept-new\n\n";

    file << "# --- options ---\n";
    file << "run_local_worker_on_master=true\n";
    file << "\n";

    file << "# --- workers (examples) ---\n";
    file << "# ip,username,worker_path[,ssh_port][,os]\n";
    file << "#10.10.10.17,kiinbang,C:\\Workers\\Test_master_worker.exe,22,windows\n";
    file << "#10.10.10.18,kiinbang,/Users/kiinbang/Workers/Test_master_worker,22,mac\n";
    file << "#10.10.10.19,kiinbang,/home/kiinbang/Workers/Test_master_worker,22,linux\n";
}

void ConfigManager::ensureConfig(const std::string& config_file) {
    std::ifstream f(config_file);
    if (!f.is_open()) {
        createDefaultConfig(config_file);
    }
}

std::vector<RemoteWorkerConfig> ConfigManager::loadRemoteWorkers(const std::string& config_file) {
    std::vector<RemoteWorkerConfig> workers;
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::wcout << L"Config file not found: " << utf8_to_wstring(config_file)
            << L". Running with local workers only.\n";
        return workers;
    }
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.find(',') != std::string::npos) {
            auto tokens = split(line, ',');
            if (tokens.size() >= 3) {
                RemoteWorkerConfig cfg;
                cfg.ip_address = trim(tokens[0]);
                cfg.username = trim(tokens[1]);
                cfg.worker_path = trim(tokens[2]);
                if (tokens.size() >= 4) cfg.ssh_port = std::stoi(trim(tokens[3]));
                workers.push_back(cfg);
                std::wcout << L"Loaded remote worker: " << utf8_to_wstring(cfg.ip_address)
                    << L" (" << utf8_to_wstring(cfg.username) << L")\n";
            }
        }
    }
    return workers;
}

RuntimeSettings ConfigManager::loadRuntimeSettings(const std::string& config_file) {
    RuntimeSettings s;
    std::ifstream file(config_file);
    if (!file.is_open()) return s;

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.find('=') == std::string::npos || line.find(',') != std::string::npos) continue;

        auto pos = line.find('=');
        std::string k = trim(line.substr(0, pos));
        std::string v = trim(line.substr(pos + 1));

        auto lcase = [](std::string t) {
            std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {return std::tolower(c); });
            return t;
            };
        std::string lk = lcase(k), lv = lcase(v);

        try {
            if (lk == "drain_seconds") s.drain_seconds = std::stoi(v);
            else if (lk == "worker_idle_timeout_seconds") s.worker_idle_timeout_seconds = std::stoi(v);
            else if (lk == "worker_retry_max") s.worker_retry_max = std::stoi(v);
            else if (lk == "worker_retry_backoff_ms") s.worker_retry_backoff_ms = std::stoi(v);
            else if (lk == "worker_recv_timeout_ms") s.worker_recv_timeout_ms = std::stoi(v);
            else if (lk == "worker_send_timeout_ms") s.worker_send_timeout_ms = std::stoi(v);
            else if (lk == "ssh_connect_timeout_sec") s.ssh_connect_timeout_sec = std::stoi(v);
            else if (lk == "ssh_server_alive_interval_sec") s.ssh_server_alive_interval_sec = std::stoi(v);
            else if (lk == "ssh_server_alive_count_max") s.ssh_server_alive_count_max = std::stoi(v);
            else if (lk == "ssh_batch_mode") s.ssh_batch_mode = (lv == "1" || lv == "true" || lv == "yes" || lv == "on");
            else if (lk == "ssh_strict_host_key") s.ssh_strict_host_key = v;
            else if (lk == "run_local_worker_on_master") {
                if (lv == "1" || lv == "true") s.run_local_worker_on_master = true;
                else s.run_local_worker_on_master = false;
            }
            else if (lk == "master_starvation_seconds") s.master_starvation_seconds = std::stoi(v);
            else if (lk == "emergency_local_spawn_max") s.emergency_local_spawn_max = std::stoi(v);
        }
        catch (...) {
            std::wcout << L"[Config] Failed to parse key: " << k.c_str() << L"\n";
        }
    }
    return s;
}