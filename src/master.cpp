#include "master.h"
#include "worker_config.h"
#include "ssh_exec.h"
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

void run_master(const std::string& ip, int port) {
    auto exe_path = fs::current_path().string();
    auto workers = load_remote_workers(exe_path);

    if (workers.empty()) {
        std::cerr << "[ERROR] No workers available.\n";
        return;
    }

    for (auto& w : workers) {
        if (w.host == "127.0.0.1" || w.host == "localhost") {
            // 🚀 로컬 워커는 직접 실행
            std::string local_cmd = "\"" + w.exe_path + "\" worker " + ip + " " + std::to_string(port) + " workers.conf";
            std::cout << "[INFO] Execute local worker: " << local_cmd << "\n";
            system(local_cmd.c_str());
        }
        else {
            // 🌐 원격 워커는 SSH 실행
            std::string cmd = build_remote_exec(w, ip, port, exe_path + "/workers.conf");
            std::cout << "[INFO] Execute remote worker: " << cmd << "\n";
            system(cmd.c_str());
        }
    }

}
