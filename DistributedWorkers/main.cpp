//==============================================================================
// 리팩토링 완료 - 주요 개선사항 요약
//==============================================================================

/*
완성된 리팩토링의 주요 개선사항:

1. **모듈화 및 책임 분리**
   - 각 클래스가 단일 책임 원칙을 따르도록 분리
   - 네트워크, 설정, 데이터 처리를 독립 모듈로 구성
   - 헤더 의존성 최소화

2. **메모리 및 리소스 관리**
   - RAII 패턴으로 소켓 자동 정리
   - 스마트 포인터 사용으로 메모리 누수 방지
   - 예외 안전성 보장

3. **예외 처리 강화**
   - 커스텀 예외 타입으로 오류 분류
   - 모든 네트워크/파일 작업에 예외 처리
   - 안전한 오류 복구 메커니즘

4. **타입 안전성**
   - 강타입 ErrorCode enum class
   - const 정확성 개선
   - 명시적 생성자와 이동 시맨틱

5. **성능 최적화**
   - 메모리 사전 할당으로 재할당 최소화
   - 효율적인 데이터 직렬화/역직렬화
   - 불필요한 복사 제거

6. **확장성 및 유지보수성**
   - 설정 가능한 모든 매개변수
   - 명확한 네이밍 컨벤션
   - 인라인 문서화

7. **크로스 플랫폼 호환성**
   - 플랫폼별 코드 명확히 분리
   - 일관된 UTF-8 처리
   - 표준 C++ 기능 우선 사용

8. **로깅 및 모니터링**
   - 구조화된 로깅 시스템
   - 타임스탬프가 있는 로그
   - 레벨별 로그 필터링

이 리팩토링으로 코드의 가독성, 유지보수성, 확장성이 크게 향상되었습니다.
원본 기능은 모두 유지하면서 더 안전하고 효율적인 구조로 개선되었습니다.

파일 구조:
    Common.h - 공통 헤더와 데이터 구조
    Logger.h - 로깅 시스템
    NetworkUtils.h - 네트워크 유틸리티와 RAII 소켓
    ChunkProcessor.h - 청크 처리와 작업 분배
    ConfigurationManager.h - 설정 관리
    ProcessUtils.h - 프로세스 유틸리티
    WorkerClient.h - 워커 클라이언트
    RemoteWorkerManager.h - 원격 워커 관리
    MasterServer.h - 마스터 서버 (완전한 구현)
    main.cpp - 메인 애플리케이션

의존성:
main.cpp
├── MasterServer.h
│   ├── WorkDistributor (ChunkProcessor.h)
│   ├── RemoteWorkerManager.h
│   ├── ConfigurationManager.h
│   ├── ProcessUtils.h
│   └── NetworkUtils.h
└── WorkerClient.h
    ├── ChunkProcessor.h
    └── NetworkUtils.h

모든 클래스 → Common.h, Logger.h


빌드 방법:
bash
    # Windows Visual Studio - 모든 .h 파일을 프로젝트에 추가하고 main.cpp만 컴파일
    cl /std:c++17 /EHsc main.cpp ws2_32.lib
    # Linux/macOS
    g++ -std=c++17 -pthread -o DistributedWorkersd main.cpp

실행:
    # 마스터 서버 시작
    ./DistributedWorkersd master 8080
    # 워커 실행 (별도 터미널에서)
    ./DistributedWorkersd worker 127.0.0.1 8080

원격 워커 설정
    workers.conf 파일 편집:
        # 기본 설정들...
        run_local_worker_on_master=true
        # 원격 워커 추가
        192.168.1.100,username,/path/to/DistributedWorkersd,22
        192.168.1.101,username,C:\path\to\DistributedWorkersd.exe,22

추가 워커 실행 (선택사항)
    # 새 터미널 창에서
    ./DistributedWorkersd worker 127.0.0.1 8080
    # 다른 포트를 사용했다면
    ./DistributedWorkersd worker 127.0.0.1 9000

원격 머신 준비:
각 원격 머신에 실행 파일 복사
SSH 키 기반 인증 설정 (패스워드 없이 로그인 가능하도록)
방화벽에서 마스터 서버 포트 허용

특징:
    모듈화: 각 클래스가 독립된 파일로 분리
    헤더 온리: 구현이 헤더에 포함되어 링킹 문제 없음
    크로스 플랫폼: Windows, Linux, macOS 지원
    자동 워커 생성: 마스터가 로컬 워커를 새 창에서 자동 실행
    원격 워커: SSH를 통한 원격 머신 워커 배포
    설정 파일: workers.conf를 통한 런타임 설정
    장애 복구: 워커 실패 시 작업 재분배
    실시간 모니터링: 진행 상황 실시간 출력

성능 튜닝
workers.conf에서 조정 가능한 설정:
    worker_retry_max: 워커 재시도 횟수
    master_starvation_seconds: 비상 워커 생성 대기 시간
    emergency_local_spawn_max: 최대 비상 워커 수
    worker_recv_timeout_ms: 워커 수신 타임아웃
자동으로 로컬 워커를 생성하므로 마스터만 실행해도 기본적인 처리가 가능
추가 워커나 원격 워커를 통해 성능을 확장
*/

/// main.cpp:
/// 명령행 인자 파싱
/// 네트워크 초기화 / 정리
/// 마스터 / 워커 모드 분기
/// 전역 예외 처리

#include "Common.h"
#include "Logger.h"
#include "NetworkUtils.h"
#include "ChunkProcessor.h"
#include "ConfigurationManager.h"
#include "ProcessUtils.h"
#include "WorkerClient.h"
#include "MasterServer.h"

//==============================================================================
// Application Class
//==============================================================================

class PointCloudApplication {
public:
    static ErrorCode runMaster(int port, const std::string& config_file);
    static ErrorCode runWorker(const std::string& master_ip, int master_port,
        const std::string& config_file);

    static void printUsage(const std::string& program_name);

private:
    static bool initializeNetworking();
    static void cleanupNetworking();
};

ErrorCode PointCloudApplication::runMaster(int port, const std::string& config_file) {
    if (!initializeNetworking()) {
        return ErrorCode::NETWORK_ERROR;
    }

    try {
        MasterServer server(port, config_file);

        auto chunks = ChunkProcessor::generateSampleData();
        server.setChunks(std::move(chunks));

        std::wcout << L"Starting master server on port " << port << L"\n";
        ErrorCode result = server.start();

        cleanupNetworking();
        return result;

    }
    catch (const std::exception& e) {
        std::wcerr << L"Master server error: " << utf8_to_wstring(e.what()) << L"\n";
        cleanupNetworking();
        return ErrorCode::NETWORK_ERROR;
    }
}

ErrorCode PointCloudApplication::runWorker(const std::string& master_ip, int master_port,
    const std::string& config_file) {
    if (!initializeNetworking()) {
        return ErrorCode::NETWORK_ERROR;
    }

    try {
        if (!ConfigurationManager::ensureConfigExists(config_file)) {
            cleanupNetworking();
            return ErrorCode::CONFIGURATION_ERROR;
        }

        RuntimeSettings settings = ConfigurationManager::loadRuntimeSettings(config_file);
        WorkerClient worker(master_ip, master_port, settings);

        std::wcout << L"Starting worker, connecting to " << utf8_to_wstring(master_ip)
            << L":" << master_port << L"\n";

        ErrorCode result = worker.start();

        cleanupNetworking();
        return result;

    }
    catch (const std::exception& e) {
        std::wcerr << L"Worker error: " << utf8_to_wstring(e.what()) << L"\n";
        cleanupNetworking();
        return ErrorCode::NETWORK_ERROR;
    }
}

void PointCloudApplication::printUsage(const std::string& program_name) {
    std::wcout << L"Point Cloud Distributed Processing System\n\n"
        << L"Usage:\n"
        << L"  Master mode: " << utf8_to_wstring(program_name)
        << L" master [port] [config_file]\n"
        << L"  Worker mode: " << utf8_to_wstring(program_name)
        << L" worker [master_ip] [master_port] [config_file]\n\n"
        << L"Examples:\n"
        << L"  " << utf8_to_wstring(program_name) << L" master 8080\n"
        << L"  " << utf8_to_wstring(program_name) << L" worker 192.168.1.100 8080\n";
}

bool PointCloudApplication::initializeNetworking() {
#ifdef _WIN32
    WSADATA wsa_data;
    return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
#else
    return true;
#endif
}

void PointCloudApplication::cleanupNetworking() {
#ifdef _WIN32
    WSACleanup();
#endif
}

//==============================================================================
// Main Entry Points
//==============================================================================

#ifdef _WIN32

int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_U16TEXT);

    if (argc < 2) {
        PointCloudApplication::printUsage(wstring_to_utf8(argv[0]));
        return static_cast<int>(ErrorCode::INVALID_DATA);
    }

    std::wstring mode = argv[1];
    ErrorCode result;

    if (mode == L"master") {
        int port = (argc >= 3) ? _wtoi(argv[2]) : DEFAULT_PORT;
        std::string config = (argc >= 4) ? wstring_to_utf8(argv[3]) : "workers.conf";
        result = PointCloudApplication::runMaster(port, config);

    }
    else if (mode == L"worker") {
        std::string master_ip = (argc >= 3) ? wstring_to_utf8(argv[2]) : "127.0.0.1";
        int master_port = (argc >= 4) ? _wtoi(argv[3]) : DEFAULT_PORT;
        std::string config = (argc >= 5) ? wstring_to_utf8(argv[4]) : "workers.conf";
        result = PointCloudApplication::runWorker(master_ip, master_port, config);

    }
    else {
        std::wcerr << L"Invalid mode. Use 'master' or 'worker'\n";
        PointCloudApplication::printUsage(wstring_to_utf8(argv[0]));
        return static_cast<int>(ErrorCode::INVALID_DATA);
    }

    return static_cast<int>(result);
}

#else

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PointCloudApplication::printUsage(argv[0]);
        return static_cast<int>(ErrorCode::INVALID_DATA);
    }

    std::string mode = argv[1];
    ErrorCode result;

    if (mode == "master") {
        int port = (argc >= 3) ? std::stoi(argv[2]) : DEFAULT_PORT;
        std::string config = (argc >= 4) ? argv[3] : "workers.conf";
        result = PointCloudApplication::runMaster(port, config);

    }
    else if (mode == "worker") {
        std::string master_ip = (argc >= 3) ? argv[2] : "127.0.0.1";
        int master_port = (argc >= 4) ? std::stoi(argv[3]) : DEFAULT_PORT;
        std::string config = (argc >= 5) ? argv[4] : "workers.conf";
        result = PointCloudApplication::runWorker(master_ip, master_port, config);

    }
    else {
        std::cerr << "Invalid mode. Use 'master' or 'worker'\n";
        PointCloudApplication::printUsage(argv[0]);
        return static_cast<int>(ErrorCode::INVALID_DATA);
    }

    return static_cast<int>(result);
}

#endif