/// 시스템 전체의 로깅 관리
/// 타임스탬프가 포함된 구조화된 로그 출력
/// 로그 레벨 필터링(DEBUG, INFO, WARN, ERROR)
/// 스레드 안전한 로그 출력
/// Logger::info(L"메시지") 형태로 일관된 로깅

#pragma once

#include "Common.h"

class Logger {
public:
    enum class Level {
        DEBUG = 0,
        INFO = 1,
        WARN = 2,
        _ERROR = 3
    };

    static void setLevel(Level level);
    static void log(Level level, const std::wstring& message);

    static void debug(const std::wstring& msg) { log(Level::DEBUG, msg); }
    static void info(const std::wstring& msg) { log(Level::INFO, msg); }
    static void warn(const std::wstring& msg) { log(Level::WARN, msg); }
    static void error(const std::wstring& msg) { log(Level::_ERROR, msg); }

private:
    static Level current_level_;
    static std::mutex log_mutex_;
    static std::wstring levelToString(Level level);
};

// Logger.cpp 구현부
Logger::Level Logger::current_level_ = Logger::Level::INFO;
std::mutex Logger::log_mutex_;

void Logger::setLevel(Level level) {
    current_level_ = level;
}

void Logger::log(Level level, const std::wstring& message) {
    if (level < current_level_) return;

    std::lock_guard<std::mutex> lock(log_mutex_);

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

#ifdef _WIN32
    // Windows에서 안전한 localtime_s 사용
    struct tm timeinfo;
    if (localtime_s(&timeinfo, &time_t) == 0) {
        std::wcout << L"[" << levelToString(level) << L"] "
            << std::put_time(&timeinfo, L"%H:%M:%S")
            << L"." << std::setfill(L'0') << std::setw(3) << ms.count()
            << L" " << message << L"\n";
    }
    else {
        std::wcout << L"[" << levelToString(level) << L"] " << message << L"\n";
    }
#else
    // Linux/macOS에서는 기존 localtime 사용
    std::wcout << L"[" << levelToString(level) << L"] "
        << std::put_time(std::localtime(&time_t), L"%H:%M:%S")
        << L"." << std::setfill(L'0') << std::setw(3) << ms.count()
        << L" " << message << L"\n";
#endif
}

std::wstring Logger::levelToString(Level level) {
    switch (level) {
    case Level::DEBUG: return L"DEBUG";
    case Level::INFO:  return L"INFO ";
    case Level::WARN:  return L"WARN ";
    case Level::_ERROR: return L"ERROR";
    default:           return L"UNKN ";
    }
}