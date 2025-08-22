#pragma once
#include <string>
#include "worker_config.h"
#include "utils.h"

#ifdef _WIN32
#include <windows.h>
#endif

// Keep inline in header to avoid multiple definitions
inline std::string build_remote_exec(const RemoteWorkerConfig& cfg,
    const std::string& master_ip,
    int master_port,
    const std::string& worker_conf_path)
{
    std::string exe = escapeDQ(cfg.exe_path);
    std::string cmd = "ssh -p " + std::to_string(cfg.port);
    cmd += " -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2";
    cmd += " -o StrictHostKeyChecking=accept-new -o BatchMode=yes ";
#ifdef _WIN32
    cmd += cfg.user + "@" + cfg.host + " \"" + exe +
           " worker " + master_ip + " " + std::to_string(master_port) + " " + escapeDQ(worker_conf_path) + "\" < NUL";
#else
    cmd += "-n " + cfg.user + "@" + cfg.host + " '" + exe +
           " worker " + master_ip + " " + std::to_string(master_port) + " " + worker_conf_path + "'";
#endif
    return cmd;
}
