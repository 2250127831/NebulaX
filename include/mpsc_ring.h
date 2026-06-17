#pragma once

#include <atomic>
#include <cstdint>

// MPSC 环形缓冲区：多生产者、单消费者、无锁。
//
// 两阶段提交：
//   1. alloc() → fetch_add 分配槽位 + spin 等待槽位空闲
//   2. 写数据到 handleData()
//   3. commit(handle) → 原子写 seq 标记就绪
//
// 消费者顺序检查 seq，保证 FIFO。seq=0 表示空闲。
//
// T: 元素类型
// CAPACITY: 槽位数

template<typename T, size_t CAPACITY>
class MPSCRing {
    struct Slot {
        T data{};
        alignas(64) std::atomic<uint64_t> seq{0};  // 0=空闲；非0=已发布的序列号
    };

    Slot slots_[CAPACITY]{};
    alignas(64) std::atomic<uint64_t> claim_idx_{0};

    uint64_t read_cursor_ = 1;  // 消费者期望的下一个 seq，永不溢出

public:
    // ── 生产者 ──

    // 分配槽位，返回句柄
    uint64_t alloc() {
        uint64_t idx = claim_idx_.fetch_add(1, std::memory_order_relaxed);
        Slot& s = slots_[idx % CAPACITY];
        while (s.seq.load(std::memory_order_acquire) != 0);
        s.seq.store(UINT64_MAX, std::memory_order_release);  // 占用标记
        return idx;
    }

    void commit(uint64_t handle) {
        Slot& s = slots_[handle % CAPACITY];
        s.seq.store(handle + 1, std::memory_order_release);
    }

    T* handleData(uint64_t handle) {
        return &slots_[handle % CAPACITY].data;
    }

    // ── 消费者 ──

    T* tryPop() {
        Slot& s = slots_[(read_cursor_ - 1) % CAPACITY];
        if (s.seq.load(std::memory_order_acquire) != read_cursor_)
            return nullptr;
        return &s.data;
    }

    void consume() {
        Slot& s = slots_[(read_cursor_ - 1) % CAPACITY];
        s.seq.store(0, std::memory_order_release);
        read_cursor_++;
    }
};
