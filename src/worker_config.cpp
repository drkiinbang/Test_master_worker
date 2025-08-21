#include "worker_config.h"
#include "utils.h"
#include <fstream>
#include <iostream>

std::vector<RemoteWorkerConfig> load_remote_workers(const std::string& filename) {
    std::vector<RemoteWorkerConfig> workers;
    std::ifstream fin(filename);
    if (!fin) {
        std::cerr << "[ERROR] Cannot open workers.conf: " << filename << "\n";
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
