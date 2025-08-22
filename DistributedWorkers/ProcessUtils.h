/// 크로스 플랫폼 프로세스 관리
/// 실행 파일 경로 자동 탐지
/// 새 터미널 창에서 로컬 워커 실행
/// 플랫폼별 명령어 생성(Windows / macOS / Linux)
/// 주요기능 : startLocalWorkerNewWindow() - 자동 워커 생성

#pragma once

#include "Common.h"

class ProcessUtils {
public:
    static std::string getSelfExecutablePath();
    static ErrorCode startLocalWorkerNewWindow(const std::string& exe_path,
        const std::string& master_ip,
        int master_port);

private:
    static std::string escapeShellArgument(const std::string& arg);
};

//==============================================================================
// ProcessUtils 구현부
//==============================================================================

std::string ProcessUtils::getSelfExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length;

    do {
        length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::string();
        }

        if (length < buffer.size()) {
            return wstring_to_utf8(std::wstring(buffer.data(), length));
        }

        buffer.resize(buffer.size() * 2);
    } while (buffer.size() < 32768);

    return std::string();

#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);

    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::string();
    }

    char resolved[PATH_MAX];
    if (realpath(buffer.data(), resolved)) {
        return std::string(resolved);
    }

    return std::string(buffer.data());

#else
    std::vector<char> buffer(4096);
    ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);

    if (length <= 0) {
        return std::string();
    }

    buffer[length] = '\0';

    // PATH_MAX 대신 동적 할당 사용
    std::vector<char> resolved_buffer(4096);
    if (realpath(buffer.data(), resolved_buffer.data())) {
        return std::string(resolved_buffer.data());
    }

    return std::string(buffer.data());
#endif
}

ErrorCode ProcessUtils::startLocalWorkerNewWindow(const std::string& exe_path,
    const std::string& master_ip,
    int master_port) {
    std::string base_command = "\"" + exe_path + "\" worker " +
        master_ip + " " + std::to_string(master_port);

#ifdef _WIN32
    std::string command = "cmd /c start \"Worker\" " + base_command;

#elif defined(__APPLE__)
    std::string escaped_command = escapeShellArgument(base_command + "; exec bash -i");
    std::string command = "osascript -e 'tell application \"Terminal\" to do script \"" +
        "bash -lc \\\"" + escaped_command + "\\\"\"' " +
        "-e 'tell application \"Terminal\" to activate'";

#else
    std::string escaped_command = escapeShellArgument(base_command + "; exec bash -i");
    std::string command =
        "if command -v gnome-terminal >/dev/null 2>&1; then "
        "gnome-terminal -- bash -lc \"" + escaped_command + "\"; "
        "elif command -v konsole >/dev/null 2>&1; then "
        "konsole --hold -e bash -lc \"" + escaped_command + "\"; "
        "elif command -v xterm >/dev/null 2>&1; then "
        "xterm -hold -e \"" + escaped_command + "\"; "
        "else "
        "tmux new-session -d -s pointcloud_worker \"" + escaped_command + "\"; "
        "echo 'Started worker in tmux session: pointcloud_worker'; "
        "fi";
#endif

    int result = std::system(command.c_str());
    return (result == 0) ? ErrorCode::SUCCESS : ErrorCode::NETWORK_ERROR;
}

std::string ProcessUtils::escapeShellArgument(const std::string& arg) {
    std::string escaped;
    escaped.reserve(arg.size() * 2);

    for (char c : arg) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        escaped += c;
    }

    return escaped;
}