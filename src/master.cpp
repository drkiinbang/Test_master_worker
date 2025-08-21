#include "master.h"
#include "worker_config.h"
#include "ssh_exec.h"
#include <iostream>

void run_master(const std::string& ip, int port, const std::string& workers_conf) {
    auto workers = load_remote_workers(workers_conf);
    for (auto& w : workers) {
        std::string cmd = build_remote_exec(w, ip, port, workers_conf);
        std::cout << "[INFO] Execute: " << cmd << "\n";
        system(cmd.c_str());
    }
}
