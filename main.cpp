#include "master.h"
#include "worker.h"
#include "config.h"
#include "common.h"

#include <iostream>
#include <string>

// Cross-platform headers for main function
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#endif

/// ---------- main / wmain ----------
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::wcerr << L"WSAStartup failed\n"; return 1;
    }

    if (argc < 2) {
        std::wcout << L"Usage:\n";
        std::wcout << L"  Master mode: " << argv[0] << L" master [port] [config_file]\n";
        std::wcout << L"  Worker mode: " << argv[0] << L" worker [master_ip] [master_port] [config_file]\n";
        WSACleanup(); return 1;
    }

    std::wstring mode = argv[1];
    if (mode == L"master") {
        int port = (argc >= 3) ? _wtoi(argv[2]) : 8080;
        std::string cfg = (argc >= 4) ? wstring_to_utf8(argv[3]) : "workers.conf";
        MasterServer server(port, cfg);
        server.generateSampleData();
        server.start();
    }
    else if (mode == L"worker") {
        std::wstring master_ip_w = (argc >= 3) ? argv[2] : L"127.0.0.1";
        int master_port = (argc >= 4) ? _wtoi(argv[3]) : 8080;
        std::string cfg = (argc >= 5) ? wstring_to_utf8(argv[4]) : "workers.conf";
        ConfigManager::ensureConfig(cfg);
        RuntimeSettings s = ConfigManager::loadRuntimeSettings(cfg);
        WorkerClient worker(wstring_to_utf8(master_ip_w), master_port, s);
        worker.start();
    }
    else {
        std::wcerr << L"Invalid mode. Use 'master' or 'worker'\n";
        WSACleanup(); return 1;
    }

    WSACleanup();
    return 0;
}
#else
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  Master mode: " << argv[0] << " master [port] [config_file]\n";
        std::cout << "  Worker mode: " << argv[0] << " worker [master_ip] [master_port] [config_file]\n";
        return 1;
    }
    std::string mode = argv[1];
    if (mode == "master") {
        int port = (argc >= 3) ? std::stoi(argv[2]) : 8080;
        std::string cfg = (argc >= 4) ? argv[3] : "workers.conf";
        MasterServer server(port, cfg);
        server.generateSampleData();
        server.start();
    }
    else if (mode == "worker") {
        std::string ip = (argc >= 3) ? argv[2] : "127.0.0.1";
        int port = (argc >= 4) ? std::stoi(argv[3]) : 8080;
        std::string cfg = (argc >= 5) ? argv[4] : "workers.conf";
        ConfigManager::ensureConfig(cfg);
        RuntimeSettings s = ConfigManager::loadRuntimeSettings(cfg);
        WorkerClient worker(ip, port, s);
        worker.start();
    }
    else {
        std::cerr << "Invalid mode. Use 'master' or 'worker'\n";
        return 1;
    }
    return 0;
}
#endif