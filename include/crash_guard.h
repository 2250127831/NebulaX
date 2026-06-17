#pragma once

#include <csignal>
#include <unistd.h>
#include <fcntl.h>

// 崩溃信号 handler：
// - fdatasync(WAL) 确保最近的操作落盘
// - _exit() 退出，不 shm_unlink（共享内存保留供重启恢复）
//
// WAL fd 由 main() 注册（WalWriter 内部 fd 不公开，维护一个全局 fd）

class CrashGuard {
public:
    static void install(int wal_fd) {
        wal_fd_ = wal_fd;
        signal(SIGSEGV, handler);
        signal(SIGABRT, handler);
        signal(SIGBUS,  handler);
    }

private:
    static void handler(int sig) {
        write(STDERR_FILENO, "CRASH: syncing WAL...\n", 22);
        if (wal_fd_ >= 0)
            fdatasync(wal_fd_);
        _exit(128 + sig);
    }

    inline static int wal_fd_ = -1;
};
