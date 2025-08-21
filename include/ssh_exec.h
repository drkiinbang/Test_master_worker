#pragma once
#include <string>
#include "worker_config.h"

std::string build_remote_exec(const RemoteWorkerConfig& cfg,
    const std::string& master_ip,
    int master_port,
    const std::string& worker_conf);
