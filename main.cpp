///////////////////////////////////////////////////////////////////////////////
/// Point Cloud Distributed Processing System
///
/// 이 프로그램은 Master-Worker 패턴으로 Point Cloud 데이터를 분산처리
/// - Master: TCP 서버로서 청크 데이터를 워커에게 전달하고 결과를 수집
/// - Worker: Master에 접속하여 할당받은 청크를 처리하고 결과를 반환
/// - 원격 워커 자동 실행: (선택) SSH로 원격 머신에 워커 프로세스를 실행
///
/// 설계상 중요 포인트:
/// 1) 네트워크는 TCP 기반. 메시지 앞에 4바이트 정수형 변수로 전달될 메세지 길이 정보 제공
/// 2) Cross-Platform: Windows/Linux 모두 동작하도록 소켓 및 IP 탐지 코드를 분기 처리
/// 3) 원격 워커 자동 실행은 OpenSSH 클라이언트(ssh)가 PATH에 있다고 가정
/// 4) 설정 파일(workers.conf)로 원격 워커 목록을 관리
///
/// Point Cloud Distributed Processing System (Enhanced)
/// - Draining phase after all results: sends TERMINATE to late workers
/// - Worker self-termination on no-more-work/no-response
/// - SSH non-interactive options; timeouts/retries configurable via config
///////////////////////////////////////////////////////////////////////////////
/// [Q] 모든 작업완료 전에, 모든 woker가 종료되면 어떻게 되지?
/// [A] 모든 chunk가 끝나기 전에 워커가 전부 종료되면, 마스터는 accept 루프에서 계속 대기함
/// draining은 모든 결과를 다 받았을 때만 켜지므로 아직 draining으로도 못 넘어가고 멈춰 있게 됨
/// 다만 원격 워커가 설정돼 있고 RemoteWorkerManager가 돌고 있으면, 
/// 그 쓰레드가 주기적으로 다시 워커를 띄우기 때문에 곧 새 워커가 접속해 이어서 처리하게 됨
/// 로컬 워커만 가동된 경우(한 번만 새 창으로 워커 실행) : 워커가 닫히면 
/// 자동 재시작이 없어서 마스터는 새 접속이 오기 전까지 진행이 무한히 멈추게 됨 (수정될 예정)
/// 

/// Silence deprecation warnings for <codecvt> on MSVC
/// This macro musb be defined before including <codecvt>
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING 1


#include "Network.h"
#include "Master.h"
#include "WorkerClient.h"
#include "Worker.h"

/// Wide characters for Windows console (not used in Linux)
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/// ---------- main / wmain ----------
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::wcerr << L"WSAStartup failed\n"; return 1;
    }

    if (argc < 2) {
        std::wcout << L"Usage:\n";
        std::wcout << L"  Master mode: " << argv[0] << L" master [port] [config_file]\n";
        std::wcout << L"  Worker mode: " << argv[0] << L" worker [master_ip] [master_port] [config_file]\n";
        WSACleanup(); return 1;
    }

    std::wstring mode = argv[1];
    if (mode == L"master") {
        int port = (argc >= 3) ? _wtoi(argv[2]) : 8080;
        std::string cfg = (argc >= 4) ? wstring_to_utf8(argv[3]) : "workers.conf";
        MasterServer server(port, cfg);
        server.setSampleData(generateSampleData());
        server.start();
    }
    else if (mode == L"worker") {
        std::wstring master_ip_w = (argc >= 3) ? argv[2] : L"127.0.0.1";
        int master_port = (argc >= 4) ? _wtoi(argv[3]) : 8080;
        std::string cfg = (argc >= 5) ? wstring_to_utf8(argv[4]) : "workers.conf";
        ConfigManager::ensureConfig(cfg);
        RuntimeSettings s = ConfigManager::loadRuntimeSettings(cfg);
        WorkerClient worker(wstring_to_utf8(master_ip_w), master_port, s);
        worker.start();
    }
    else {
        std::wcerr << L"Invalid mode. Use 'master' or 'worker'\n";
        WSACleanup(); return 1;
    }

    WSACleanup();
    return 0;
}
#else
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  Master mode: " << argv[0] << " master [port] [config_file]\n";
        std::cout << "  Worker mode: " << argv[0] << " worker [master_ip] [master_port] [config_file]\n";
        return 1;
    }
    std::string mode = argv[1];
    if (mode == "master") {
        int port = (argc >= 3) ? std::stoi(argv[2]) : 8080;
        std::string cfg = (argc >= 4) ? argv[3] : "workers.conf";
        MasterServer server(port, cfg);
        server.setSampleData(generateSampleData());
        server.start();
    }
    else if (mode == "worker") {
        std::string ip = (argc >= 3) ? argv[2] : "127.0.0.1";
        int port = (argc >= 4) ? std::stoi(argv[3]) : 8080;
        std::string cfg = (argc >= 5) ? argv[4] : "workers.conf";
        ConfigManager::ensureConfig(cfg);
        RuntimeSettings s = ConfigManager::loadRuntimeSettings(cfg);
        WorkerClient worker(ip, port, s);
        worker.start();
    }
    else {
        std::cerr << "Invalid mode. Use 'master' or 'worker'\n";
        return 1;
    }
    return 0;
}
#endif
