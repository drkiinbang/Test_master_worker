#include <iostream>
#include <string>
#include "master.h"
#include "worker.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage:\n"
            << "  " << argv[0] << " master <ip> <port> <workers.conf>\n"
            << "  " << argv[0] << " worker <master_ip> <master_port> <worker.conf>\n";
        return 1;
    }

    std::string mode = argv[1];
    if (mode == "master" && argc >= 5) {
        run_master(argv[2], std::stoi(argv[3]), argv[4]);
    }
    else if (mode == "worker" && argc >= 5) {
        run_worker(argv[2], std::stoi(argv[3]), argv[4]);
    }
    else {
        std::cerr << "[ERROR] Invalid arguments\n";
        return 1;
    }

    return 0;
}
