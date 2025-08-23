/// 워커 프로세스의 메인 로직
/// 마스터 서버 연결 및 재연결
/// 청크 데이터 수신 및 처리
/// 결과 전송 및 오류 처리
/// 타임아웃 및 재시도 로직
/// 주요기능 : start() - 워커의 메인 루프

#pragma once

#include "Common.h"
#include "NetworkUtils.h"
#include "ChunkProcessor.h"
#include "ConfigurationManager.h"

class WorkerClient {
public:
    WorkerClient(const std::string& master_ip, int master_port,
        const std::string& config_file = "worker.config");

    ErrorCode start();
    void stop();

private:
    bool connectAndProcessSingle();
    SOCKET createConnection();

    const std::string master_ip_;
    const int master_port_;
    const std::string config_file_;

    WorkerSettings settings_;
    std::atomic<bool> should_stop_{ false };
    bool last_was_terminate_{ false };
    int consecutive_failures_{ 0 };
    std::chrono::steady_clock::time_point last_success_;
};

//==============================================================================
// WorkerClient 구현부
//==============================================================================

WorkerClient::WorkerClient(const std::string& master_ip, int master_port,
    const std::string& config_file)
    : master_ip_(master_ip), master_port_(master_port), config_file_(config_file),
    last_success_(std::chrono::steady_clock::now()) {

    // 설정 파일 로드
    if (!ConfigurationManager::ensureWorkerConfigExists(config_file_)) {
        // 기본 설정 사용
        settings_ = WorkerSettings{};
    }
    else {
        settings_ = ConfigurationManager::loadWorkerSettings(config_file_);
    }
}

ErrorCode WorkerClient::start() {
    std::wcout << L"Worker connecting to " << utf8_to_wstring(master_ip_)
        << L":" << master_port_ << L"\n";

    while (!should_stop_) {
        if (!connectAndProcessSingle()) {
            if (last_was_terminate_) {
                std::wcout << L"Worker terminated by master\n";
                return ErrorCode::SUCCESS;
            }

            auto now = std::chrono::steady_clock::now();
            auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_success_).count();

            if (idle_duration >= settings_.worker_idle_timeout_seconds ||
                ++consecutive_failures_ >= settings_.worker_retry_max) {
                std::wcout << L"Worker timeout or max retries reached. Exiting.\n";
                return ErrorCode::TIMEOUT;
            }

            int backoff_ms = settings_.worker_retry_backoff_ms * consecutive_failures_;
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            continue;
        }

        consecutive_failures_ = 0;
        last_success_ = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return ErrorCode::SUCCESS;
}

void WorkerClient::stop() {
    should_stop_ = true;
}

bool WorkerClient::connectAndProcessSingle() {
    RAIISocket socket(createConnection());
    if (!socket.valid()) {
        return false;
    }

    try {
        std::string received_data = NetworkUtils::receiveData(socket.get());
        if (received_data.empty()) {
            return false;
        }

        if (NetworkUtils::isTerminationSignal(received_data)) {
            last_was_terminate_ = true;
            return false;
        }

        PointCloudChunk chunk = PointCloudChunk::deserialize(received_data);
        std::wcout << L"Processing chunk " << chunk.chunk_id
            << L" (" << chunk.size() << L" points)\n";

        ProcessResult result = ChunkProcessor::processChunk(chunk);

        if (!NetworkUtils::sendData(socket.get(), result.serialize())) {
            return false;
        }

        std::wcout << L"Completed chunk " << chunk.chunk_id
            << L" (avg_distance: " << result.avg_distance << L")\n";
        return true;

    }
    catch (const std::exception& e) {
        std::wcerr << L"Error processing chunk: " << utf8_to_wstring(e.what()) << L"\n";
        return false;
    }
}

SOCKET WorkerClient::createConnection() {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    if (!NetworkUtils::setSocketTimeouts(socket,
        settings_.worker_recv_timeout_ms,
        settings_.worker_send_timeout_ms)) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(master_port_);

    if (inet_pton(AF_INET, master_ip_.c_str(), &addr.sin_addr) != 1) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    if (::connect(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    return socket;
}