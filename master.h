#pragma once
#pragma once

#include "common.h"
#include "config.h"
#include "network.h"
#include <vector>
#include <mutex>

static int startLocalWorkerNewWindow(const std::string& exePath,
    const std::string& master_ip,
    int master_port);

class RemoteWorkerManager {
private:
    std::vector<RemoteWorkerConfig> remote_configs;
    std::vector<std::thread> remote_threads;
    std::string master_ip;
    int master_port;
    RuntimeSettings settings;
    std::atomic<bool> should_stop{ false };

public:
    RemoteWorkerManager(const std::vector<RemoteWorkerConfig>& configs,
        const std::string& master_ip, int master_port,
        const RuntimeSettings& settings);
    ~RemoteWorkerManager();
    void startRemoteWorkers();
    void stop();
private:
    void runRemoteWorker(const RemoteWorkerConfig& config);
};

class MasterServer {
private:
    std::vector<PointCloudChunk> chunks;
    std::vector<ProcessResult>   results;
    std::atomic<int>             chunk_index{ 0 };
    std::mutex                   results_mutex;
    int                          port;
    std::atomic<bool>            all_chunks_processed{ false };
    std::unique_ptr<RemoteWorkerManager> remote_manager;
    std::string                  config_file;
    RuntimeSettings              settings;
    std::vector<RemoteWorkerConfig> remote_configs;
    std::deque<int> retry_queue_;
    std::mutex      retry_mtx_;
    std::unordered_set<int> completed_chunk_ids_;
    std::chrono::steady_clock::time_point last_progress_{ std::chrono::steady_clock::now() };
    int emergency_local_spawned_ = 0;

public:
    MasterServer(int p, const std::string& config = "workers.conf");
    ~MasterServer();
    void generateSampleData();
    bool has_pending_work() const;
    void start();

private:
    void printFinalResults();
    bool try_acquire_next_chunk(int& out_cid);
    void requeue_chunk_if_needed(int cid);
};