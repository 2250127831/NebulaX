#pragma once

#include <cstdint>
#include <cstddef>

struct WalEntry {
    uint8_t  type;
    uint8_t  side;
    uint32_t price;
    uint32_t quantity;
    uint64_t user_id;
    uint64_t order_id;
    uint64_t wal_seq;
};
static_assert(sizeof(WalEntry) == 40, "WalEntry must be 40 bytes");

// WAL: 固定 512MB 环形文件
static constexpr size_t WAL_SIZE      = 512UL << 20;
static constexpr size_t WAL_ENTRIES   = WAL_SIZE / sizeof(WalEntry);

class WalWriter {
public:
    bool init(const char* path = "/tmp/nebulaX_wal.dat");
    void append(const WalEntry& entry);
    uint64_t totalWritten() const { return total_; }
    int fd() const { return fd_; }

    // 返回是否需要做 checkpoint（刚完成一轮循环，pos 回到 0）
    // 调用方应在此之后 fork checkpoint
    bool needCheckpoint() const { return total_ > 0 && total_ % WAL_ENTRIES == 0; }

    // 当前物理写入位置（用于 checkpoint 恢复时定位）
    // 即总条数对 WAL_ENTRIES 取模
    size_t curPosition() const { return total_ % WAL_ENTRIES; }

    void close();

private:
    int fd_ = -1;
    WalEntry* base_ = nullptr;
    uint64_t total_ = 0;     // 总写入条数（单调递增，不重置）
};

class WalReader {
public:
    bool open(const char* path);
    size_t entryCount() const { return count_; }
    const WalEntry* entryAt(size_t i) const { return (i < count_) ? &base_[i] : nullptr; }
    void close();
private:
    int fd_ = -1;
    WalEntry* base_ = nullptr;
    size_t count_ = 0;
};
