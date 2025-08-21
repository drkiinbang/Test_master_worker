#include "network.h"
#include <iostream>
#include <vector>

bool NetworkUtils::sendData(SOCKET s, const std::string& data) {
    uint32_t size = static_cast<uint32_t>(data.size());
    if (send(s, reinterpret_cast<const char*>(&size), sizeof(size), 0) != sizeof(size)) return false;
    const char* buf = data.data();
    int total = 0, need = static_cast<int>(size);
    while (total < need) {
        int sent = send(s, buf + total, need - total, 0);
        if (sent == SOCKET_ERROR) return false;
        total += sent;
    }
    return true;
}

std::string NetworkUtils::receiveData(SOCKET s) {
    uint32_t size = 0;
    int recvd = recv(s, reinterpret_cast<char*>(&size), sizeof(size), 0);
    if (recvd != sizeof(size)) return "";
    std::string data(size, '\0');
    int total = 0, need = static_cast<int>(size);
    while (total < need) {
        int got = recv(s, &data[total], need - total, 0);
        if (got == SOCKET_ERROR || got == 0) return "";
        total += got;
    }
    return data;
}

bool NetworkUtils::sendTerminationSignal(SOCKET s) {
    static const std::string k = "TERMINATE";
    return sendData(s, k);
}
bool NetworkUtils::isTerminationSignal(const std::string& d) {
    return d == "TERMINATE";
}