#pragma once

#include <cstdint>
#include <atomic>
#include <thread>
#include "mpsc_ring.h"

// ── 日志级别 ──
enum LogLevel : uint8_t {
    LOG_FATAL = 0,
    LOG_ERROR = 1,
    LOG_WARN  = 2,
    LOG_INFO  = 3,
};

const char* levelStr(LogLevel l);

struct LogEntry {
    LogLevel level;
    char     msg[255];  // vsnprintf 输出
};

// ── Logger 单例 ──
class Logger {
public:
    static Logger& instance();

    void init(const char* dir, LogLevel threshold);
    void log(LogLevel level, const char* fmt, ...);
    void shutdown();

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void consumerLoop();

    MPSCRing<LogEntry, 4096> ring_;

    int wake_fd_ = -1;
    int log_fd_  = -1;
    LogLevel threshold_ = LOG_INFO;
    std::thread consumer_;
    std::atomic<bool> stopped_{false};
};

// ── 宏 API ──
#define LOG_FATAL(...)  Logger::instance().log(LOG_FATAL, __VA_ARGS__)
#define LOG_ERROR(...)  Logger::instance().log(LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...)   Logger::instance().log(LOG_WARN, __VA_ARGS__)
#define LOG_INFO(...)   Logger::instance().log(LOG_INFO, __VA_ARGS__)
