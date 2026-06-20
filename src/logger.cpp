#include "logger.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <sys/eventfd.h>

const char* levelStr(LogLevel l) {
    switch (l) {
        case LOG_FATAL: return "FATAL";
        case LOG_ERROR: return "ERROR";
        case LOG_WARN:  return "WARN";
        case LOG_INFO:  return "INFO";
        default:        return "?";
    }
}

static void formatTimestamp(char* buf, size_t len) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int n = strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
    if (n > 0 && n < (int)len - 4)
        snprintf(buf + n, len - n, ".%03ld", ts.tv_nsec / 1000000);
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const char* dir, LogLevel threshold) {
    threshold_ = threshold;

    char mkdir_cmd[256];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dir);
    system(mkdir_cmd);

    char path[512];
    snprintf(path, sizeof(path), "%s/nebulaX.log", dir);
    log_fd_ = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (log_fd_ < 0) {
        write(STDERR_FILENO, "ERROR: failed to open log file\n", 31);
        return;
    }

    wake_fd_ = eventfd(0, 0);
    if (wake_fd_ < 0) {
        write(STDERR_FILENO, "ERROR: eventfd() failed for logger\n", 35);
        close(log_fd_);
        log_fd_ = -1;
        return;
    }

    consumer_ = std::thread(&Logger::consumerLoop, this);
}

void Logger::log(LogLevel level, const char* fmt, ...) {
    if (level > threshold_) return;
    if (log_fd_ < 0) return;

    uint64_t handle = ring_.alloc();
    auto* entry = ring_.handleData(handle);
    entry->level = level;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(entry->msg, sizeof(LogEntry::msg), fmt, ap);
    va_end(ap);
    ring_.commit(handle);
    // 通知消费者（eventfd 累积计数，多次通知只产生一次唤醒）
    if (wake_fd_ >= 0) {
        uint64_t one = 1;
        write(wake_fd_, &one, sizeof(one));
    }
}

void Logger::shutdown() {
    stopped_.store(true, std::memory_order_release);

    if (wake_fd_ >= 0) {
        uint64_t val = 1;
        write(wake_fd_, &val, sizeof(val));
    }

    if (consumer_.joinable())
        consumer_.join();

    if (log_fd_ >= 0) {
        char ts[32];
        formatTimestamp(ts, sizeof(ts));
        while (auto* e = ring_.tryPop()) {
            dprintf(log_fd_, "[%s] [%s] %s\n", ts, levelStr(e->level), e->msg);
            ring_.consume();
        }
        close(log_fd_);
        log_fd_ = -1;
    }

    if (wake_fd_ >= 0) {
        close(wake_fd_);
        wake_fd_ = -1;
    }
}

void Logger::consumerLoop() {
    char ts[32];

    while (!stopped_.load(std::memory_order_acquire)) {
        auto* entry = ring_.tryPop();
        if (!entry) {
            uint64_t val;
            read(wake_fd_, &val, 8);
            continue;
        }

        formatTimestamp(ts, sizeof(ts));
        dprintf(log_fd_, "[%s] [%s] %s\n", ts, levelStr(entry->level), entry->msg);
        ring_.consume();

        // 同批剩余条目，共享同一时间戳
        while ((entry = ring_.tryPop()) != nullptr) {
            dprintf(log_fd_, "[%s] [%s] %s\n", ts, levelStr(entry->level), entry->msg);
            ring_.consume();
        }
    }
}
