#pragma once

#include "common.h"
#include "config.h"
#include <string>

class WorkerClient {
private:
    std::string master_ip;
    int master_port;
    RuntimeSettings settings;
    bool last_was_terminate_ = false;

public:
    WorkerClient(const std::string& ip, int port, const RuntimeSettings& s);
    void start();

private:
    bool connectAndProcess();
    ProcessResult processPointCloud(const PointCloudChunk& chunk);
};