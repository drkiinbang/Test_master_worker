#pragma once
#include <string>
#include "worker_config.h"
#include "utils.h"

#ifdef _WIN32
#include <windows.h>
#endif

std::string build_remote_exec(const RemoteWorkerConfig& cfg,
    const std::string& master_ip,
    int master_port,
    const std::string& worker_conf)
{
    std::string cmd;
    std::string exe = escapeDQ(cfg.exe_path);

#ifdef _WIN32
    cmd = "ssh -p " + std::to_string(cfg.port);
#else
    cmd = "ssh -p " + std::to_string(cfg.port);
#endif

    cmd += " -o StrictHostKeyChecking=accept-new";
    cmd += " " + cfg.user + "@" + cfg.host;
    cmd += " \"" + exe + " worker " + master_ip + " " + std::to_string(master_port) + " " + worker_conf + "\"";
    return cmd;
}
