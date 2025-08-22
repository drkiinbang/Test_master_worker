/// 작업 분배 및 상태 관리
/// 청크 할당 / 재할당 로직
/// 완료된 작업 추적
/// 실패한 작업의 재큐잉
/// 스레드 안전한 작업 상태 관리
/// 주요기능 : tryAcquireChunk(), markCompleted()
/// ----------------------------------------------------------------
/// 샘플연산 (테스트용)
/// 포인트 클라우드 청크의 평균 거리 계산
/// 샘플 데이터 생성(테스트용)
/// 작업 단위 정의 및 처리
/// 주요기능 : processChunk() - 핵심 계산 로직

#pragma once

#include "Common.h"

//==============================================================================
// Chunk Processor
//==============================================================================

class ChunkProcessor {
public:
    static ProcessResult processChunk(const PointCloudChunk& chunk);
    static std::vector<PointCloudChunk> generateSampleData(int chunk_count = DEFAULT_CHUNK_COUNT,
        int points_per_chunk = MAX_CHUNK_SIZE);

private:
    static float calculateAverageDistance(const std::vector<Point3D>& points);
};

//==============================================================================
// ChunkProcessor 구현부
//==============================================================================

ProcessResult ChunkProcessor::processChunk(const PointCloudChunk& chunk) {
    if (chunk.empty()) {
        return ProcessResult(chunk.chunk_id, 0.0f, 0);
    }

    float avg_distance = calculateAverageDistance(chunk.points);
    return ProcessResult(chunk.chunk_id, avg_distance,
        static_cast<int>(chunk.points.size()));
}

std::vector<PointCloudChunk> ChunkProcessor::generateSampleData(int chunk_count,
    int points_per_chunk) {
    std::vector<PointCloudChunk> chunks;
    chunks.reserve(chunk_count);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

    for (int i = 0; i < chunk_count; ++i) {
        PointCloudChunk chunk(i);
        for (int j = 0; j < points_per_chunk; ++j) {
            chunk.points.emplace_back(dist(gen), dist(gen), dist(gen));
        }
        chunks.push_back(std::move(chunk));
    }

    std::wcout << L"Generated " << chunk_count << L" chunks with "
        << chunk_count * points_per_chunk << L" total points\n";

    return chunks;
}

float ChunkProcessor::calculateAverageDistance(const std::vector<Point3D>& points) {
    if (points.empty()) return 0.0f;

    float total_distance = 0.0f;
    for (const auto& point : points) {
        total_distance += point.distanceFromOrigin();
    }

    return total_distance / static_cast<float>(points.size());
}

//==============================================================================
// Work Distributor
//==============================================================================

class WorkDistributor {
public:
    explicit WorkDistributor(std::vector<PointCloudChunk> chunks);

    bool tryAcquireChunk(int& chunk_id);
    void requeueChunk(int chunk_id);
    void markCompleted(int chunk_id);

    bool hasRemainingWork() const;
    size_t getTotalChunks() const;
    size_t getCompletedCount() const;

    const PointCloudChunk& getChunk(int chunk_id) const;

private:
    std::vector<PointCloudChunk> chunks_;
    std::atomic<int> next_chunk_index_{ 0 };

    mutable std::mutex state_mutex_;
    std::deque<int> retry_queue_;
    std::unordered_set<int> completed_chunks_;
};

//==============================================================================
// WorkDistributor 구현부
//==============================================================================

WorkDistributor::WorkDistributor(std::vector<PointCloudChunk> chunks)
    : chunks_(std::move(chunks)) {
}

bool WorkDistributor::tryAcquireChunk(int& chunk_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!retry_queue_.empty()) {
        chunk_id = retry_queue_.front();
        retry_queue_.pop_front();
        return true;
    }

    int index = next_chunk_index_.fetch_add(1);
    if (index < static_cast<int>(chunks_.size())) {
        chunk_id = index;
        return true;
    }

    return false;
}

void WorkDistributor::requeueChunk(int chunk_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (completed_chunks_.find(chunk_id) == completed_chunks_.end()) {
        retry_queue_.push_back(chunk_id);
    }
}

void WorkDistributor::markCompleted(int chunk_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    completed_chunks_.insert(chunk_id);
}

bool WorkDistributor::hasRemainingWork() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return completed_chunks_.size() < chunks_.size();
}

size_t WorkDistributor::getTotalChunks() const {
    return chunks_.size();
}

size_t WorkDistributor::getCompletedCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return completed_chunks_.size();
}

const PointCloudChunk& WorkDistributor::getChunk(int chunk_id) const {
    if (chunk_id < 0 || chunk_id >= static_cast<int>(chunks_.size())) {
        throw std::out_of_range("Invalid chunk ID: " + std::to_string(chunk_id));
    }
    return chunks_[chunk_id];
}