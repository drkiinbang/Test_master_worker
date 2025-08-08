///////////////////////////////////////////////////////////////////////////////
/// Point Cloud Distributed Processing System - Cross Platform Compatible
/// 
/// 크로스 플랫폼 호환성 개선사항:
/// - SOCKET 타입 정의 통일
/// - 네트워크 초기화/정리 함수 분리
/// - 플랫폼별 헤더 및 라이브러리 처리
/// - 에러 처리 통일
/// - IP 주소 변환 함수 통일
///////////////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <thread>
#include <string>
#include <sstream>
#include <chrono>
#include <random>
#include <atomic>
#include <mutex>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <iomanip>  // setprecision을 위해 추가

// 플랫폼별 네트워크 헤더 및 타입 정의
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Windows용 타입 정의
typedef int socklen_t;
#define close closesocket
#define SOCKET_ERROR_VAL SOCKET_ERROR
#define INVALID_SOCKET_VAL INVALID_SOCKET

#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>

// Linux용 타입 정의 (Windows와 호환)
typedef int SOCKET;
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR_VAL SOCKET_ERROR
#define INVALID_SOCKET_VAL INVALID_SOCKET

#endif

/// 크로스 플랫폼 네트워크 초기화/정리 클래스
class NetworkInit {
public:
    static bool initialize() {
#ifdef _WIN32
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
        return true; // Linux는 초기화 불필요
#endif
    }

    static void cleanup() {
#ifdef _WIN32
        WSACleanup();
#else
        // Linux는 정리 불필요
#endif
    }

    static int getLastError() {
#ifdef _WIN32
        return WSAGetLastError();
#else
        return errno;
#endif
    }

    static std::string getErrorString(int error) {
#ifdef _WIN32
        char buffer[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, error, 0, buffer, sizeof(buffer), NULL);
        return std::string(buffer);
#else
        return std::string(strerror(error));
#endif
    }
};

/// MSVC용 구조체 패킹 설정
#pragma pack(push, 1)

/// Point3D 구조체 - 바이너리 전송에 최적화
struct Point3D {
    float x, y, z;
    Point3D(float x = 0, float y = 0, float z = 0) : x(x), y(y), z(z) {}
};

/// 바이너리 직렬화를 위한 헤더 구조체
struct ChunkHeader {
    int32_t chunk_id;
    int32_t point_count;
    int32_t data_size;  // 실제 포인트 데이터 크기
};

/// 결과 헤더 구조체
struct ResultHeader {
    int32_t chunk_id;
    float avg_distance;
    int32_t point_count;
};

#pragma pack(pop)  // 패킹 설정 복원

/// 바이너리 직렬화가 가능한 Point Cloud 청크
class PointCloudChunk {
public:
    int chunk_id;
    std::vector<Point3D> points;

    /// 바이너리 직렬화 - 효율적인 메모리 사용
    std::vector<uint8_t> serializeBinary() const {
        ChunkHeader header;
        header.chunk_id = chunk_id;
        header.point_count = static_cast<int32_t>(points.size());
        header.data_size = static_cast<int32_t>(points.size() * sizeof(Point3D));

        // 전체 데이터 크기 계산
        size_t total_size = sizeof(ChunkHeader) + header.data_size;
        std::vector<uint8_t> buffer(total_size);

        // 헤더 복사
        std::memcpy(buffer.data(), &header, sizeof(ChunkHeader));

        // 포인트 데이터 복사
        if (!points.empty()) {
            std::memcpy(buffer.data() + sizeof(ChunkHeader),
                points.data(), header.data_size);
        }

        return buffer;
    }

    /// 바이너리 역직렬화
    static PointCloudChunk deserializeBinary(const std::vector<uint8_t>& data) {
        PointCloudChunk chunk;

        if (data.size() < sizeof(ChunkHeader)) {
            throw std::runtime_error("Invalid chunk data: too small");
        }

        // 헤더 읽기
        ChunkHeader header;
        std::memcpy(&header, data.data(), sizeof(ChunkHeader));

        chunk.chunk_id = header.chunk_id;

        // 데이터 크기 검증
        if (data.size() != sizeof(ChunkHeader) + header.data_size) {
            throw std::runtime_error("Invalid chunk data: size mismatch");
        }

        // 포인트 데이터 읽기
        if (header.point_count > 0) {
            chunk.points.resize(header.point_count);
            std::memcpy(chunk.points.data(),
                data.data() + sizeof(ChunkHeader),
                header.data_size);
        }

        return chunk;
    }

    /// 기존 텍스트 직렬화 (호환성 유지)
    std::string serialize() const {
        std::stringstream ss;
        ss << chunk_id << " " << points.size() << " ";
        for (const auto& p : points) {
            ss << p.x << " " << p.y << " " << p.z << " ";
        }
        return ss.str();
    }

    static PointCloudChunk deserialize(const std::string& data) {
        PointCloudChunk chunk;
        std::stringstream ss(data);
        size_t size;
        ss >> chunk.chunk_id >> size;

        chunk.points.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            Point3D p;
            ss >> p.x >> p.y >> p.z;
            chunk.points.push_back(p);
        }
        return chunk;
    }
};

/// 바이너리 직렬화가 가능한 처리 결과
class ProcessResult {
public:
    int chunk_id;
    float avg_distance;
    int point_count;

    /// 바이너리 직렬화
    std::vector<uint8_t> serializeBinary() const {
        std::vector<uint8_t> buffer(sizeof(ResultHeader));

        ResultHeader header;
        header.chunk_id = chunk_id;
        header.avg_distance = avg_distance;
        header.point_count = point_count;

        std::memcpy(buffer.data(), &header, sizeof(ResultHeader));
        return buffer;
    }

    /// 바이너리 역직렬화
    static ProcessResult deserializeBinary(const std::vector<uint8_t>& data) {
        if (data.size() != sizeof(ResultHeader)) {
            throw std::runtime_error("Invalid result data size");
        }

        ProcessResult result;
        ResultHeader header;
        std::memcpy(&header, data.data(), sizeof(ResultHeader));

        result.chunk_id = header.chunk_id;
        result.avg_distance = header.avg_distance;
        result.point_count = header.point_count;

        return result;
    }

    /// 기존 텍스트 직렬화 (호환성 유지)
    std::string serialize() const {
        std::stringstream ss;
        ss << chunk_id << " " << avg_distance << " " << point_count;
        return ss.str();
    }

    static ProcessResult deserialize(const std::string& data) {
        ProcessResult result;
        std::stringstream ss(data);
        ss >> result.chunk_id >> result.avg_distance >> result.point_count;
        return result;
    }
};

/// 향상된 네트워크 유틸리티 클래스 - 바이너리 데이터 지원
class NetworkUtils {
public:
    /// 프로토콜 타입 정의
    enum class ProtocolType : uint8_t {
        BINARY_CHUNK = 1,
        BINARY_RESULT = 2,
        TEXT_DATA = 3,
        TERMINATION = 255
    };

    /// 크로스 플랫폼 IP 주소 변환
    static bool stringToAddr(const std::string& ip, struct sockaddr_in& addr) {
#ifdef _WIN32
        return InetPtonA(AF_INET, ip.c_str(), &addr.sin_addr) == 1;
#else
        return inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) == 1;
#endif
    }

    /// 안전한 소켓 닫기
    static void closeSocket(SOCKET sock) {
        if (sock != INVALID_SOCKET_VAL) {
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
        }
    }

    /// 바이너리 데이터 전송
    static bool sendBinaryData(SOCKET socket, const std::vector<uint8_t>& data) {
        uint32_t size = static_cast<uint32_t>(data.size());

        // 크기 전송
        if (send(socket, reinterpret_cast<const char*>(&size), sizeof(size), 0) != sizeof(size)) {
            std::cerr << "Failed to send size. Error: " << NetworkInit::getLastError() << std::endl;
            return false;
        }

        // 데이터 전송
        const char* buffer = reinterpret_cast<const char*>(data.data());
        size_t totalSent = 0;
        size_t dataSize = data.size();

        while (totalSent < dataSize) {
            int sent = send(socket, buffer + totalSent, static_cast<int>(dataSize - totalSent), 0);
            if (sent == SOCKET_ERROR_VAL) {
                std::cerr << "Failed to send data. Error: " << NetworkInit::getLastError() << std::endl;
                return false;
            }
            totalSent += sent;
        }
        return true;
    }

    /// 바이너리 데이터 수신
    static std::vector<uint8_t> receiveBinaryData(SOCKET socket) {
        uint32_t size;

        // 크기 수신
        int received = recv(socket, reinterpret_cast<char*>(&size), sizeof(size), 0);
        if (received != sizeof(size)) {
            if (received == 0) {
                std::cout << "Connection closed by peer" << std::endl;
            }
            else if (received == SOCKET_ERROR_VAL) {
                std::cerr << "Failed to receive size. Error: " << NetworkInit::getLastError() << std::endl;
            }
            return {};
        }

        // 데이터 수신
        std::vector<uint8_t> data(size);
        size_t totalReceived = 0;

        while (totalReceived < size) {
            int received = recv(socket, reinterpret_cast<char*>(data.data()) + totalReceived,
                static_cast<int>(size - totalReceived), 0);
            if (received == SOCKET_ERROR_VAL || received == 0) {
                std::cerr << "Failed to receive data. Error: " << NetworkInit::getLastError() << std::endl;
                return {};
            }
            totalReceived += received;
        }

        return data;
    }

    /// 기존 텍스트 전송 (호환성 유지)
    static bool sendData(SOCKET socket, const std::string& data) {
        uint32_t size = static_cast<uint32_t>(data.length());

        if (send(socket, reinterpret_cast<const char*>(&size), sizeof(size), 0) != sizeof(size)) {
            std::cerr << "Failed to send text size. Error: " << NetworkInit::getLastError() << std::endl;
            return false;
        }

        const char* buffer = data.c_str();
        size_t totalSent = 0;
        size_t dataSize = data.length();

        while (totalSent < dataSize) {
            int sent = send(socket, buffer + totalSent, static_cast<int>(dataSize - totalSent), 0);
            if (sent == SOCKET_ERROR_VAL) {
                std::cerr << "Failed to send text data. Error: " << NetworkInit::getLastError() << std::endl;
                return false;
            }
            totalSent += sent;
        }
        return true;
    }

    static std::string receiveData(SOCKET socket) {
        uint32_t size;

        int received = recv(socket, reinterpret_cast<char*>(&size), sizeof(size), 0);
        if (received != sizeof(size)) {
            if (received == 0) {
                std::cout << "Connection closed by peer" << std::endl;
            }
            else if (received == SOCKET_ERROR_VAL) {
                std::cerr << "Failed to receive text size. Error: " << NetworkInit::getLastError() << std::endl;
            }
            return "";
        }

        std::string data(size, '\0');
        size_t totalReceived = 0;

        while (totalReceived < size) {
            int received = recv(socket, &data[totalReceived], static_cast<int>(size - totalReceived), 0);
            if (received == SOCKET_ERROR_VAL || received == 0) {
                std::cerr << "Failed to receive text data. Error: " << NetworkInit::getLastError() << std::endl;
                return "";
            }
            totalReceived += received;
        }

        return data;
    }

    /// 프로토콜 타입과 함께 데이터 전송
    static bool sendWithProtocol(SOCKET socket, ProtocolType type, const std::vector<uint8_t>& data) {
        // 프로토콜 타입 전송
        if (send(socket, reinterpret_cast<const char*>(&type), sizeof(type), 0) != sizeof(type)) {
            std::cerr << "Failed to send protocol type. Error: " << NetworkInit::getLastError() << std::endl;
            return false;
        }

        // 데이터 전송
        return sendBinaryData(socket, data);
    }

    /// 프로토콜 타입과 함께 데이터 수신 (C++14 호환)
    static bool receiveWithProtocol(SOCKET socket, ProtocolType& outType, std::vector<uint8_t>& outData) {
        // 프로토콜 타입 수신
        int received = recv(socket, reinterpret_cast<char*>(&outType), sizeof(outType), 0);
        if (received != sizeof(outType)) {
            if (received == 0) {
                std::cout << "Connection closed by peer" << std::endl;
            }
            else if (received == SOCKET_ERROR_VAL) {
                std::cerr << "Failed to receive protocol type. Error: " << NetworkInit::getLastError() << std::endl;
            }
            return false;
        }

        // 데이터 수신
        outData = receiveBinaryData(socket);
        return !outData.empty() || outType == ProtocolType::TERMINATION;
    }

    /// 종료 신호 전송
    static bool sendTerminationSignal(SOCKET socket) {
        ProtocolType type = ProtocolType::TERMINATION;
        if (send(socket, reinterpret_cast<const char*>(&type), sizeof(type), 0) != sizeof(type)) {
            std::cerr << "Failed to send termination type. Error: " << NetworkInit::getLastError() << std::endl;
            return false;
        }

        uint32_t size = 0;
        return send(socket, reinterpret_cast<const char*>(&size), sizeof(size), 0) == sizeof(size);
    }
};

/// 성능 측정 유틸리티
class PerformanceMonitor {
private:
    std::chrono::high_resolution_clock::time_point start_time;
    size_t binary_bytes_sent = 0;
    size_t text_bytes_sent = 0;

public:
    void startMeasurement() {
        start_time = std::chrono::high_resolution_clock::now();
        binary_bytes_sent = 0;
        text_bytes_sent = 0;
    }

    void addBinaryBytes(size_t bytes) { binary_bytes_sent += bytes; }
    void addTextBytes(size_t bytes) { text_bytes_sent += bytes; }

    void printResults() {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        std::cout << "\n=== Performance Results ===\n";
        std::cout << "Processing time: " << duration.count() << " ms\n";

        if (binary_bytes_sent > 0) {
            std::cout << "Binary data sent: " << binary_bytes_sent << " bytes\n";
            if (duration.count() > 0) {
                std::cout << "Transfer rate: " << (binary_bytes_sent * 1000) / duration.count() << " bytes/sec\n";
            }
        }

        if (text_bytes_sent > 0) {
            std::cout << "Text data would be: " << text_bytes_sent << " bytes\n";
            if (text_bytes_sent > 0) {
                double savings = ((double)(text_bytes_sent - binary_bytes_sent) / text_bytes_sent) * 100;
                std::cout << "Data size reduction: " << std::fixed << std::setprecision(1)
                    << savings << "%\n";
            }
        }
    }
};

/// 개선된 Master 서버 - 바이너리 전송 지원
class MasterServer {
private:
    std::vector<PointCloudChunk> chunks;
    std::vector<ProcessResult> results;
    std::atomic<int> chunk_index;
    std::mutex results_mutex;
    int port;
    std::atomic<bool> all_chunks_processed;
    PerformanceMonitor perf_monitor;
    bool use_binary_protocol;

public:
    MasterServer(int p, bool binary = true)
        : port(p), chunk_index(0), all_chunks_processed(false), use_binary_protocol(binary) {
    }

    void generateSampleData() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> dis(-100.0f, 100.0f);

        for (int chunk_id = 0; chunk_id < 10; ++chunk_id) {
            PointCloudChunk chunk;
            chunk.chunk_id = chunk_id;

            for (int i = 0; i < 10000; ++i) { // 더 많은 데이터로 성능 차이 확인
                chunk.points.emplace_back(dis(gen), dis(gen), dis(gen));
            }
            chunks.push_back(chunk);
        }

        std::cout << "Generated " << chunks.size() << " chunks with total "
            << chunks.size() * 10000 << " points\n";
        std::cout << "Using " << (use_binary_protocol ? "BINARY" : "TEXT")
            << " protocol\n";
    }

    void start() {
        perf_monitor.startMeasurement();

        if (!NetworkInit::initialize()) {
            std::cerr << "Network initialization failed\n";
            return;
        }

        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock == INVALID_SOCKET_VAL) {
            std::cerr << "Socket creation failed. Error: " << NetworkInit::getLastError() << std::endl;
            NetworkInit::cleanup();
            return;
        }

        int opt = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (bind(server_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR_VAL) {
            std::cerr << "Bind failed. Error: " << NetworkInit::getLastError() << std::endl;
            NetworkUtils::closeSocket(server_sock);
            NetworkInit::cleanup();
            return;
        }

        if (listen(server_sock, 10) == SOCKET_ERROR_VAL) {
            std::cerr << "Listen failed. Error: " << NetworkInit::getLastError() << std::endl;
            NetworkUtils::closeSocket(server_sock);
            NetworkInit::cleanup();
            return;
        }

        std::cout << "Master server listening on port " << port << "\n";

        while (!all_chunks_processed.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);

            SOCKET client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_sock == INVALID_SOCKET_VAL) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            char ipStr[INET_ADDRSTRLEN];
#ifdef _WIN32
            InetNtopA(AF_INET, &client_addr.sin_addr, ipStr, INET_ADDRSTRLEN);
#else
            inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, INET_ADDRSTRLEN);
#endif
            std::cout << "Worker connected: " << ipStr << "\n";

            int current_chunk = chunk_index.fetch_add(1);
            if (current_chunk < static_cast<int>(chunks.size())) {
                if (use_binary_protocol) {
                    // 바이너리 프로토콜 사용
                    auto binary_data = chunks[current_chunk].serializeBinary();
                    if (NetworkUtils::sendWithProtocol(client_sock,
                        NetworkUtils::ProtocolType::BINARY_CHUNK,
                        binary_data)) {
                        perf_monitor.addBinaryBytes(binary_data.size() + sizeof(uint32_t) + sizeof(uint8_t));

                        // 비교용 텍스트 크기 계산
                        std::string text_version = chunks[current_chunk].serialize();
                        perf_monitor.addTextBytes(text_version.length() + sizeof(uint32_t));

                        std::cout << "Sent binary chunk " << current_chunk << " ("
                            << binary_data.size() << " bytes vs "
                            << text_version.length() << " text bytes)\n";

                        // 결과 수신 (C++14 호환 방식)
                        NetworkUtils::ProtocolType type;
                        std::vector<uint8_t> result_data;

                        if (NetworkUtils::receiveWithProtocol(client_sock, type, result_data)) {
                            if (type == NetworkUtils::ProtocolType::BINARY_RESULT && !result_data.empty()) {
                                ProcessResult result = ProcessResult::deserializeBinary(result_data);

                                {
                                    std::lock_guard<std::mutex> lock(results_mutex);
                                    results.push_back(result);
                                    std::cout << "Received binary result for chunk " << result.chunk_id
                                        << ": avg_distance=" << result.avg_distance
                                        << ", points=" << result.point_count << "\n";

                                    if (results.size() == chunks.size()) {
                                        all_chunks_processed.store(true);
                                        std::cout << "All chunks processed!\n";
                                    }
                                }
                            }
                        }
                    }
                }
                else {
                    // 기존 텍스트 프로토콜 사용
                    std::string chunk_data = chunks[current_chunk].serialize();
                    if (NetworkUtils::sendData(client_sock, chunk_data)) {
                        perf_monitor.addTextBytes(chunk_data.length() + sizeof(uint32_t));
                        std::cout << "Sent text chunk " << current_chunk << "\n";

                        std::string result_data = NetworkUtils::receiveData(client_sock);
                        if (!result_data.empty()) {
                            ProcessResult result = ProcessResult::deserialize(result_data);

                            {
                                std::lock_guard<std::mutex> lock(results_mutex);
                                results.push_back(result);
                                std::cout << "Received text result for chunk " << result.chunk_id << "\n";

                                if (results.size() == chunks.size()) {
                                    all_chunks_processed.store(true);
                                    std::cout << "All chunks processed!\n";
                                }
                            }
                        }
                    }
                }
            }
            else {
                NetworkUtils::sendTerminationSignal(client_sock);
                std::cout << "Sent termination signal to worker\n";
            }

            NetworkUtils::closeSocket(client_sock);
        }

        NetworkUtils::closeSocket(server_sock);
        printFinalResults();
        perf_monitor.printResults();
        NetworkInit::cleanup();
    }

private:
    void printFinalResults() {
        std::cout << "\n=== Final Results ===\n";
        float total_avg = 0.0f;
        int total_points = 0;

        for (const auto& result : results) {
            total_avg += result.avg_distance;
            total_points += result.point_count;
        }

        if (!results.empty()) {
            std::cout << "Overall average distance: " << total_avg / results.size() << "\n";
            std::cout << "Total points processed: " << total_points << "\n";
        }
    }
};

/// 개선된 Worker 클라이언트 - 바이너리 전송 지원
class WorkerClient {
private:
    std::string master_ip;
    int master_port;
    bool use_binary_protocol;

public:
    WorkerClient(const std::string& ip, int port, bool binary = true)
        : master_ip(ip), master_port(port), use_binary_protocol(binary) {
    }

    void start() {
        if (!NetworkInit::initialize()) {
            std::cerr << "Network initialization failed\n";
            return;
        }

        std::cout << "Worker started. Using " << (use_binary_protocol ? "BINARY" : "TEXT")
            << " protocol\n";

        while (true) {
            if (!connectAndProcess()) {
                std::cout << "Worker terminated.\n";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        NetworkInit::cleanup();
    }

private:
    bool connectAndProcess() {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET_VAL) {
            std::cerr << "Socket creation failed. Error: " << NetworkInit::getLastError() << std::endl;
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(master_port));

        if (!NetworkUtils::stringToAddr(master_ip, addr)) {
            std::cerr << "Invalid IP address: " << master_ip << std::endl;
            NetworkUtils::closeSocket(sock);
            return false;
        }

        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR_VAL) {
            std::cerr << "Connection to master failed. Error: " << NetworkInit::getLastError() << std::endl;
            NetworkUtils::closeSocket(sock);
            return false;
        }

        if (use_binary_protocol) {
            // 바이너리 프로토콜 사용 (C++14 호환 방식)
            NetworkUtils::ProtocolType type;
            std::vector<uint8_t> data;

            if (!NetworkUtils::receiveWithProtocol(sock, type, data)) {
                NetworkUtils::closeSocket(sock);
                return false;
            }

            if (type == NetworkUtils::ProtocolType::TERMINATION) {
                std::cout << "Received termination signal from master\n";
                NetworkUtils::closeSocket(sock);
                return false;
            }

            if (type == NetworkUtils::ProtocolType::BINARY_CHUNK && !data.empty()) {
                try {
                    PointCloudChunk chunk = PointCloudChunk::deserializeBinary(data);
                    std::cout << "Received binary chunk " << chunk.chunk_id
                        << " with " << chunk.points.size() << " points ("
                        << data.size() << " bytes)\n";

                    ProcessResult result = processPointCloud(chunk);
                    auto result_data = result.serializeBinary();

                    NetworkUtils::sendWithProtocol(sock,
                        NetworkUtils::ProtocolType::BINARY_RESULT,
                        result_data);
                    std::cout << "Sent binary result for chunk " << result.chunk_id << "\n";
                }
                catch (const std::exception& e) {
                    std::cerr << "Binary deserialization error: " << e.what() << "\n";
                }
            }
        }
        else {
            // 기존 텍스트 프로토콜 사용
            std::string received_data = NetworkUtils::receiveData(sock);
            if (received_data.empty()) {
                NetworkUtils::closeSocket(sock);
                return false;
            }

            if (received_data == "TERMINATE") {
                std::cout << "Received termination signal from master\n";
                NetworkUtils::closeSocket(sock);
                return false;
            }

            PointCloudChunk chunk = PointCloudChunk::deserialize(received_data);
            std::cout << "Received text chunk " << chunk.chunk_id << "\n";

            ProcessResult result = processPointCloud(chunk);
            std::string result_data = result.serialize();
            NetworkUtils::sendData(sock, result_data);
            std::cout << "Sent text result for chunk " << result.chunk_id << "\n";
        }

        NetworkUtils::closeSocket(sock);
        return true;
    }

    ProcessResult processPointCloud(const PointCloudChunk& chunk) {
        std::cout << "Processing chunk " << chunk.chunk_id << "...\n";

        float total_distance = 0.0f;
        for (const auto& point : chunk.points) {
            float distance = std::sqrt(point.x * point.x + point.y * point.y + point.z * point.z);
            total_distance += distance;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        ProcessResult result;
        result.chunk_id = chunk.chunk_id;
        result.avg_distance = total_distance / chunk.points.size();
        result.point_count = static_cast<int>(chunk.points.size());

        return result;
    }
};

/// 메인 함수
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  Master mode: " << argv[0] << " master [port] [binary|text]\n";
        std::cout << "  Worker mode: " << argv[0] << " worker [master_ip] [master_port] [binary|text]\n";
        std::cout << "\nDefault protocol: binary\n";
        std::cout << "\nExamples:\n";
        std::cout << "  " << argv[0] << " master 8080 binary\n";
        std::cout << "  " << argv[0] << " worker 127.0.0.1 8080 binary\n";
        return 1;
    }

    std::string mode = argv[1];
    bool use_binary = true;

    // 프로토콜 타입 확인
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "text") {
            use_binary = false;
            break;
        }
    }

    if (mode == "master") {
        int port = (argc >= 3) ? std::atoi(argv[2]) : 8080;

        // 포트 번호 유효성 검사
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid port number: " << port << std::endl;
            return 1;
        }

        MasterServer server(port, use_binary);
        server.generateSampleData();
        server.start();
    }
    else if (mode == "worker") {
        std::string master_ip = (argc >= 3) ? argv[2] : "127.0.0.1";
        int master_port = (argc >= 4) ? std::atoi(argv[3]) : 8080;

        // 포트 번호 유효성 검사
        if (master_port <= 0 || master_port > 65535) {
            std::cerr << "Invalid port number: " << master_port << std::endl;
            return 1;
        }

        WorkerClient worker(master_ip, master_port, use_binary);
        worker.start();
    }
    else {
        std::cerr << "Invalid mode. Use 'master' or 'worker'\n";
        return 1;
    }

    return 0;
}