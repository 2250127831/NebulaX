#include "wal.h"
#include "logger.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

bool WalWriter::init(const char* path) {
    fd_ = ::open(path, O_CREAT | O_RDWR, 0644);
    if (fd_ < 0) { LOG_ERROR("WAL open failed"); return false; }

    off_t sz = lseek(fd_, 0, SEEK_END);
    if (sz == 0) {
        ftruncate(fd_, WAL_SIZE);
        total_ = 0;
    } else {
        // 已有文件，恢复 total_
        total_ = sz / sizeof(WalEntry);
        if (total_ > WAL_ENTRIES) total_ = WAL_ENTRIES;
    }

    base_ = (WalEntry*)mmap(nullptr, WAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        LOG_ERROR("WAL mmap failed");
        ::close(fd_); fd_ = -1;
        return false;
    }
    return true;
}

void WalWriter::append(const WalEntry& entry) {
    size_t pos = total_ % WAL_ENTRIES;
    base_[pos] = entry;
    total_++;
}

void WalWriter::close() {
    if (base_) { msync(base_, WAL_SIZE, MS_SYNC); munmap(base_, WAL_SIZE); base_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

// ── WalReader ──

bool WalReader::open(const char* path) {
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) return false;

    off_t sz = lseek(fd_, 0, SEEK_END);
    if (sz < (off_t)sizeof(WalEntry)) { ::close(fd_); fd_ = -1; return false; }
    count_ = sz / sizeof(WalEntry);
    if (count_ > WAL_ENTRIES) count_ = WAL_ENTRIES;

    base_ = (WalEntry*)mmap(nullptr, WAL_SIZE, PROT_READ, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) { ::close(fd_); fd_ = -1; return false; }
    return true;
}

void WalReader::close() {
    if (base_) { munmap(base_, WAL_SIZE); base_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}
