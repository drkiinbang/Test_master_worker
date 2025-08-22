#pragma once

#include <string>

/// Cross platform headers related with socket/network
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/select.h>
/// Linux: Definition for Windows compatibility
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket close
#endif

/// <windows.h> 는 <winsock2.h> 보다 먼저 포함되면 충돌
/// <windows.h> 안에는 오래된 <winsock.h> 를 암묵적으로 포함하는 경우가 있어서, 이후 <winsock2.h> 와 상수 / 매크로 / 함수가 중복 정의되어 에러가 발생
/// 따라서, #include <winsock2.h> 를 반드시 <windows.h>보다 먼저 포함해야 함
#ifdef _WIN32
#include <windows.h>
#endif

/// 로컬 IP 자동 탐지(IPv4)
/// - Windows: getaddrinfo + inet_ntop
/// - Linux:   getifaddrs + inet_ntop (루프백 제외)
/// 반환 실패 시 127.0.0.1
/// 수정 가이드:
///  - 멀티 NIC 환경에서 특정 인터페이스 우선순위를 두고 싶다면
///    아래 Linux 분기에서 인터페이스 이름(예: "eth0")을 조건에 추가
///  - IPv6 지원이 필요하면 AF_INET6 분기와 버퍼 크기(INET6_ADDRSTRLEN) 추가
static std::string getLocalIPAddress() {
    std::string local_ip = "127.0.0.1";
#ifdef _WIN32
    char hostname[256] = { 0 };
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return local_ip;
    }
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || !result) {
        return local_ip;
    }
    char ipStr[INET_ADDRSTRLEN] = { 0 };
    auto* a = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    if (inet_ntop(AF_INET, &(a->sin_addr), ipStr, sizeof(ipStr))) {
        local_ip = ipStr;
    }
    freeaddrinfo(result);
#else
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) return local_ip;
    for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa || !ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET && !(ifa->ifa_flags & IFF_LOOPBACK)) {
            char ip[INET_ADDRSTRLEN] = { 0 };
            void* addr_ptr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
            if (inet_ntop(AF_INET, addr_ptr, ip, sizeof(ip))) {
                local_ip = ip; break;
            }
        }
    }
    freeifaddrs(ifaddr);
#endif
    return local_ip;
}

/// 네트워크 유틸리티
/// - send/recv는 길이 프레이밍을 이용합니다(먼저 4바이트 길이, 그 다음 페이로드).
/// 수정 가이드:
///  - 타임아웃이나 논블로킹 소켓을 사용하려면 select/poll/epoll을 추가로 감싸세요.
///  - 큰 데이터 전송 시 압축(zstd) 등을 추가 가능.
class NetworkUtils {
public:
    static bool sendData(SOCKET s, const std::string& data) {
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

    static std::string receiveData(SOCKET s) {
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

    static bool sendTerminationSignal(SOCKET s) {
        static const std::string k = "TERMINATE";
        return sendData(s, k);
    }
    static bool isTerminationSignal(const std::string& d) { return d == "TERMINATE"; }
};