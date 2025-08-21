#pragma once
#pragma once

#include "common.h"
#include <string>

class NetworkUtils {
public:
    static bool sendData(SOCKET s, const std::string& data);
    static std::string receiveData(SOCKET s);
    static bool sendTerminationSignal(SOCKET s);
    static bool isTerminationSignal(const std::string& d);
};