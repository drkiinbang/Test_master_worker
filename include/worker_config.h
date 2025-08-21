#pragma once
#pragma once
#include <string>
#include <vector>

struct RemoteWorkerConfig {
    std::string host;
    std::string user;
    std::string exe_path;
    int port = 22;
    std::string auth_type;  // "password" or "key"
    std::string password;
    std::string hostkey;
};

std::vector<RemoteWorkerConfig> load_remote_workers(const std::string& filename);
