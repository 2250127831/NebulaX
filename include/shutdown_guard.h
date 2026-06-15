#pragma once

#include <atomic>
#include <csignal>

// 线程安全退出管理：捕获 SIGTERM/SIGINT，各线程轮询 isStopping() 退出循环。
class ShutdownGuard {
    static inline std::atomic<bool> stopped_{false};
public:
    static void install() {
        struct sigaction sa;
        sa.sa_handler = [](int) { stopped_.store(true, std::memory_order_release); };
        sigfillset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT,  &sa, nullptr);
    }
    static bool isStopping() {
        return stopped_.load(std::memory_order_acquire);
    }
};
