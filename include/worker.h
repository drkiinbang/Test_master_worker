#pragma once
#include <string>
#include <iostream>

void run_worker(const std::string& master_ip, int master_port, const std::string& worker_conf) {
    std::cout << "[WORKER] Connected to master " << master_ip << ":" << master_port
        << " with config " << worker_conf << "\n";
    // TODO: worker logic
}
