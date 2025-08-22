#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cctype>
#include "utils.h"

namespace fs = std::filesystem;

struct RemoteWorkerConfig {
    std::string host;
    std::string user;
    std::string exe_path;
    int         port = 22;
    std::string auth_type;  // optional: "password" or "key"
    std::string password;   // optional
    std::string hostkey;    // optional

    bool is_local() const {
        std::string h = host;
        for (auto& c : h) c = (char)tolower((unsigned char)c);
        return (h == "127.0.0.1" || h == "localhost");
    }
};

// workers.conf format (CSV-style, '#' for comments):
//   host,user,exe_path,port,auth_type,password,hostkey
// Examples:
//   127.0.0.1,myuser,C:\\Workers\\Test_master_worker.exe,22
//   10.10.10.23,ubuntu,/home/ubuntu/Test_master_worker,22,key,,ssh-ed25519 AAAA...
//
inline std::vector<RemoteWorkerConfig> load_remote_workers(const std::string& base_dir) {
    std::vector<RemoteWorkerConfig> workers;
    fs::path conf = fs::path(base_dir) / "workers.conf";

    if (!fs::exists(conf)) {
        // Create a minimal template to help the user
        std::ofstream ofs(conf.string(), std::ios::out | std::ios::trunc);
        if (ofs) {
            ofs << "# workers.conf\n"
                   "# host,user,exe_path,port,auth_type,password,hostkey\n"
                   "# 127.0.0.1,localuser,C:\\\\Workers\\\\Test_master_worker.exe,22\n";
        }
        std::cerr << "[WARN ] workers.conf not found. A template file has been created at: "
                  << conf.string() << "\n";
        return workers;
    }

    std::ifstream ifs(conf.string());
    if (!ifs) {
        std::cerr << "[ERROR] Failed to open " << conf.string() << "\n";
        return workers;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        auto parts = split(line, ',');
        if (parts.size() < 3) {
            std::cerr << "[WARN ] Invalid workers.conf line (need at least host,user,exe_path): " << line << "\n";
            continue;
        }

        RemoteWorkerConfig cfg;
        cfg.host = trim(parts[0]);
        cfg.user = trim(parts[1]);
        cfg.exe_path = trim(parts[2]);

        if (parts.size() >= 4 && !trim(parts[3]).empty()) cfg.port = std::stoi(trim(parts[3]));
        if (parts.size() >= 5) cfg.auth_type = trim(parts[4]);
        if (parts.size() >= 6) cfg.password = trim(parts[5]);
        if (parts.size() >= 7) cfg.hostkey  = trim(parts[6]);

        workers.push_back(std::move(cfg));
    }

    return workers;
}
