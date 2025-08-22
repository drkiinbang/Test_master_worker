#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

/// ---------- Point / Chunk / Result ----------
struct Point3D {
    float x, y, z;
    Point3D(float x = 0, float y = 0, float z = 0) : x(x), y(y), z(z) {}
};

struct PointCloudChunk {
    int chunk_id{};
    std::vector<Point3D> points;

    std::string serialize() const {
        std::stringstream ss;
        ss << chunk_id << " " << points.size() << " ";
        for (const auto& p : points) ss << p.x << " " << p.y << " " << p.z << " ";
        return ss.str();
    }
    static PointCloudChunk deserialize(const std::string& data) {
        PointCloudChunk c; std::stringstream ss(data); size_t n = 0; ss >> c.chunk_id >> n;
        c.points.reserve(n);
        for (size_t i = 0; i < n; ++i) { Point3D p; ss >> p.x >> p.y >> p.z; c.points.push_back(p); }
        return c;
    }
};

#include <random>
/// 테스트를 위한 샘플 생성
std::vector<PointCloudChunk> generateSampleData() {
    std::vector<PointCloudChunk> chunks; /// 전송할 chunk
    std::random_device rd; std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(-100.0f, 100.0f);
    for (int cid = 0; cid < 100; ++cid) {
        PointCloudChunk c; c.chunk_id = cid;
        for (int i = 0; i < 1000; ++i) c.points.emplace_back(dis(gen), dis(gen), dis(gen));
        chunks.push_back(std::move(c));
    }
    std::wcout << L"Generated " << chunks.size() << L" chunks with total "
        << chunks.size() * 1000 << L" points\n";

    return chunks;
}
