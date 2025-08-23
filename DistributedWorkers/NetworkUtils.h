/// 저수준 네트워크 통신 추상화
/// TCP 소켓 생성, 설정, 데이터 송수신
/// 바이너리 프로토콜 구현(크기 + 데이터)
/// 종료 신호 처리(TERMINATION_MAGIC)
/// 소켓 타임아웃 및 재사용 설정
/// 주요기능 : sendData(), receiveData(), RAIISocket 자동 정리

#pragma once

#include "Common.h"

//==============================================================================
// RAII Socket Management
//==============================================================================

class RAIISocket {
public:
    explicit RAIISocket(SOCKET socket = INVALID_SOCKET) : socket_(socket) {}

    ~RAIISocket() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }

    RAIISocket(const RAIISocket&) = delete;
    RAIISocket& operator=(const RAIISocket&) = delete;

    RAIISocket(RAIISocket&& other) noexcept : socket_(other.socket_) {
        other.socket_ = INVALID_SOCKET;
    }

    RAIISocket& operator=(RAIISocket&& other) noexcept {
        if (this != &other) {
            if (socket_ != INVALID_SOCKET) {
                closesocket(socket_);
            }
            socket_ = other.socket_;
            other.socket_ = INVALID_SOCKET;
        }
        return *this;
    }

    SOCKET get() const { return socket_; }
    SOCKET release() {
        SOCKET temp = socket_;
        socket_ = INVALID_SOCKET;
        return temp;
    }

    bool valid() const { return socket_ != INVALID_SOCKET; }

private:
    SOCKET socket_;
};

//==============================================================================
// Network Utilities
//==============================================================================

class NetworkUtils {
public:
    static constexpr uint32_t TERMINATION_MAGIC = 0xDEADBEEF;

    // 기존 함수들
    static bool sendData(SOCKET socket, const std::string& data);
    static std::string receiveData(SOCKET socket);
    static bool sendTerminationSignal(SOCKET socket);
    static bool isTerminationSignal(const std::string& data);

    // 새로 추가된 함수들
    static bool sendMessage(SOCKET socket, MessageType type, const std::string& data);
    static bool receiveMessage(SOCKET socket, MessageType& type, std::string& data);
    static bool sendHeartbeat(SOCKET socket, const HeartbeatMessage& heartbeat);

    static bool setSocketTimeouts(SOCKET socket, int recv_timeout_ms, int send_timeout_ms);
    static bool setReuseAddress(SOCKET socket);
    static std::string getLocalIPAddress();

private:
    static bool sendAll(SOCKET socket, const char* data, size_t size);
    static bool receiveAll(SOCKET socket, char* buffer, size_t size);
};

//==============================================================================
// NetworkUtils 구현부
//==============================================================================

bool NetworkUtils::sendData(SOCKET socket, const std::string& data) {
    uint32_t size = static_cast<uint32_t>(data.size());

    if (!sendAll(socket, reinterpret_cast<const char*>(&size), sizeof(size))) {
        return false;
    }

    return sendAll(socket, data.data(), data.size());
}

std::string NetworkUtils::receiveData(SOCKET socket) {
    uint32_t size = 0;
    if (!receiveAll(socket, reinterpret_cast<char*>(&size), sizeof(size))) {
        return "";
    }

    std::string data(size, '\0');
    if (!receiveAll(socket, &data[0], size)) {
        return "";
    }

    return data;
}

bool NetworkUtils::sendTerminationSignal(SOCKET socket) {
    uint32_t magic = TERMINATION_MAGIC;
    return sendAll(socket, reinterpret_cast<const char*>(&magic), sizeof(magic));
}

bool NetworkUtils::isTerminationSignal(const std::string& data) {
    if (data.size() != sizeof(uint32_t)) return false;
    uint32_t magic = *reinterpret_cast<const uint32_t*>(data.data());
    return magic == TERMINATION_MAGIC;
}

bool NetworkUtils::setSocketTimeouts(SOCKET socket, int recv_timeout_ms, int send_timeout_ms) {
#ifdef _WIN32
    DWORD recv_timeout = static_cast<DWORD>(recv_timeout_ms);
    DWORD send_timeout = static_cast<DWORD>(send_timeout_ms);

    bool recv_ok = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&recv_timeout),
        sizeof(recv_timeout)) == 0;
    bool send_ok = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&send_timeout),
        sizeof(send_timeout)) == 0;

    return recv_ok && send_ok;
#else
    auto ms_to_timeval = [](int ms) {
        timeval tv{};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        return tv;
        };

    timeval recv_tv = ms_to_timeval(recv_timeout_ms);
    timeval send_tv = ms_to_timeval(send_timeout_ms);

    bool recv_ok = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
        &recv_tv, sizeof(recv_tv)) == 0;
    bool send_ok = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
        &send_tv, sizeof(send_tv)) == 0;

    return recv_ok && send_ok;
#endif
}

bool NetworkUtils::setReuseAddress(SOCKET socket) {
    int opt = 1;
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

std::string NetworkUtils::getLocalIPAddress() {
    std::string local_ip = "127.0.0.1";

#ifdef _WIN32
    char hostname[256] = { 0 };
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return local_ip;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || !result) {
        return local_ip;
    }

    char ip_str[INET_ADDRSTRLEN] = { 0 };
    auto* addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    if (inet_ntop(AF_INET, &(addr->sin_addr), ip_str, sizeof(ip_str))) {
        local_ip = ip_str;
    }

    freeaddrinfo(result);

#else
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) return local_ip;

    for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa || !ifa->ifa_addr) continue;

        if (ifa->ifa_addr->sa_family == AF_INET &&
            !(ifa->ifa_flags & IFF_LOOPBACK)) {
            char ip[INET_ADDRSTRLEN] = { 0 };
            void* addr_ptr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;

            if (inet_ntop(AF_INET, addr_ptr, ip, sizeof(ip))) {
                local_ip = ip;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
#endif

    return local_ip;
}

bool NetworkUtils::sendAll(SOCKET socket, const char* data, size_t size) {
    size_t total_sent = 0;
    while (total_sent < size) {
        int sent = send(socket, data + total_sent,
            static_cast<int>(size - total_sent), 0);
        if (sent == SOCKET_ERROR) return false;
        total_sent += sent;
    }
    return true;
}

bool NetworkUtils::receiveAll(SOCKET socket, char* buffer, size_t size) {
    size_t total_received = 0;
    while (total_received < size) {
        int received = recv(socket, buffer + total_received,
            static_cast<int>(size - total_received), 0);
        if (received == SOCKET_ERROR || received == 0) return false;
        total_received += received;
    }
    return true;
}

bool NetworkUtils::sendMessage(SOCKET socket, MessageType type, const std::string& data) {
    // 메시지 헤더: [타입(1바이트)] [크기(4바이트)] [데이터]
    uint8_t msg_type = static_cast<uint8_t>(type);
    uint32_t size = static_cast<uint32_t>(data.size());

    if (!sendAll(socket, reinterpret_cast<const char*>(&msg_type), sizeof(msg_type))) {
        return false;
    }

    if (!sendAll(socket, reinterpret_cast<const char*>(&size), sizeof(size))) {
        return false;
    }

    return sendAll(socket, data.data(), data.size());
}

bool NetworkUtils::receiveMessage(SOCKET socket, MessageType& type, std::string& data) {
    uint8_t msg_type;
    if (!receiveAll(socket, reinterpret_cast<char*>(&msg_type), sizeof(msg_type))) {
        return false;
    }

    uint32_t size;
    if (!receiveAll(socket, reinterpret_cast<char*>(&size), sizeof(size))) {
        return false;
    }

    data.resize(size);
    if (!receiveAll(socket, &data[0], size)) {
        return false;
    }

    type = static_cast<MessageType>(msg_type);
    return true;
}

bool NetworkUtils::sendHeartbeat(SOCKET socket, const HeartbeatMessage& heartbeat) {
    return sendMessage(socket, MessageType::HEARTBEAT, heartbeat.serialize());
}