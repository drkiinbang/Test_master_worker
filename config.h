#pragma once
#pragma once

#include "common.h"
#include <string>
#include <vector>

struct RemoteWorkerConfig {
    std::string ip_address;
    std::string username;
    std::string worker_path;
    int ssh_port;
    RemoteWorkerConfig() : ssh_port(22) {}
    RemoteWorkerConfig(const std::string& ip, const std::string& user,
        const std::string& path, int port = 22)
        : ip_address(ip), username(user), worker_path(path), ssh_port(port) {
    }
};

struct RuntimeSettings {
    int drain_seconds = 5;
    int worker_idle_timeout_seconds = 20;
    int worker_retry_max = 5;
    int worker_retry_backoff_ms = 200;
    int worker_recv_timeout_ms = 5000;
    int worker_send_timeout_ms = 5000;
    int  ssh_connect_timeout_sec = 5;
    int  ssh_server_alive_interval_sec = 5;
    int  ssh_server_alive_count_max = 2;
    bool ssh_batch_mode = true;
    std::string ssh_strict_host_key = "accept-new";
    bool run_local_worker_on_master = false;
    int master_starvation_seconds = 30;
    int emergency_local_spawn_max = 3;
};

class ConfigManager {
public:
    static void createDefaultConfig(const std::string& config_file);
    static void ensureConfig(const std::string& config_file);
    static std::vector<RemoteWorkerConfig> loadRemoteWorkers(const std::string& config_file);
    static RuntimeSettings loadRuntimeSettings(const std::string& config_file);
};