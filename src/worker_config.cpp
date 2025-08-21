#include "worker_config.h"
#include "utils.h"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

static const char* DEFAULT_CONF =
"# Default workers.conf generated automatically\n"
"# Format: host,username,exe_path,port,auth_type,password,hostkey\n"
"127.0.0.1,localuser,C:\\\\Workers\\\\Test_master_worker.exe,22,password,localpass,\n";

std::vector<RemoteWorkerConfig> load_remote_workers(const std::string& exe_dir) {
    std::vector<RemoteWorkerConfig> workers;
    std::string conf_path = exe_dir + "/workers.conf";

    if (!fs::exists(conf_path)) {
        std::cerr << "[WARN] workers.conf not found. Creating default: " << conf_path << "\n";
        std::ofstream fout(conf_path);
        fout << DEFAULT_CONF;
        fout.close();
    }

    std::ifstream fin(conf_path);
    if (!fin) {
        std::cerr << "[ERROR] Cannot open workers.conf even after creating default.\n";
        return workers;
    }

    std::string line;
    int lineno = 0;
    while (std::getline(fin, line)) {
        ++lineno;
        std::string raw = trim(line);
        if (raw.empty() || raw[0] == '#') continue;

        auto parts = split(raw, ',');
        if (parts.size() < 3) {
            std::cerr << "[WARN] Invalid line " << lineno << " in workers.conf\n";
            continue;
        }

        RemoteWorkerConfig cfg;
        cfg.host = trim(parts[0]);
        cfg.user = trim(parts[1]);
        cfg.exe_path = trim(parts[2]);
        if (parts.size() >= 4) cfg.port = std::stoi(trim(parts[3]));
        if (parts.size() >= 5) cfg.auth_type = trim(parts[4]);
        if (parts.size() >= 6) cfg.password = trim(parts[5]);
        if (parts.size() >= 7) cfg.hostkey = trim(parts[6]);

        workers.push_back(cfg);
    }

    return workers;
}
