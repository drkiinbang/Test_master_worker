#include "master.h"
#include "worker_config.h"
#include "ssh_exec.h"
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

void run_master(const std::string& ip, int port) {
    // 실행파일 경로 확인
    auto exe_path = fs::current_path().string();
    auto workers = load_remote_workers(exe_path);

    if (workers.empty()) {
        std::cerr << "[ERROR] No workers available.\n";
        return;
    }

    for (auto& w : workers) {
        std::string cmd = build_remote_exec(w, ip, port, exe_path + "/workers.conf");
        std::cout << "[INFO] Execute: " << cmd << "\n";
        system(cmd.c_str());
    }
}
