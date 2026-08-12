#pragma once

#include "order.h"
#include <atomic>
#include <cstdint>
#include <cstddef>

// 固定容量 Order 池，构造时决定大小，空闲链表管理。
// 单块连续数组，Order* 永远稳定，at() 零开销。
//
// 并发（迁移自 NebulaX-Trader order_pool.h）：Treiber 无锁空闲栈（带 tag 消 ABA）。
//   槽归还后立即复用，并发 alloc/free 时无 tag 会因 ABA 让两个线程同时"拥有"同一槽。
//   tag 随每次成功 CAS 递增，stale CAS 失败。free_head_ 读用 acquire，与 deallocate 的
//   release 写配对：槽被另一线程 deallocate(release) 后本线程 allocate(acquire) 看到 →
//   建立 happens-before，槽字段的写(前 owner)与读(新 owner)有序，TSAN 不误判复用为竞争。
class OrderPool
{
public:
    explicit OrderPool(size_t capacity)
        : storage_(new Order[capacity])
        , capacity_(capacity)
    {
        initFreeList();
    }

    // 使用外部 mmap 存储（共享内存）
    OrderPool(Order* external_storage, size_t capacity, bool init_free)
        : storage_(external_storage)
        , capacity_(capacity)
        , owns_storage_(false)
    {
        if (init_free) initFreeList();
    }

    ~OrderPool() { if (owns_storage_) delete[] storage_; }

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    Order* allocate()
    {
        uint64_t fh = free_head_.load(std::memory_order_acquire);
        while ((uint32_t)fh != UINT32_MAX) {
            uint32_t head = (uint32_t)fh;
            uint32_t next = storage_[head].pool_next_free.load(std::memory_order_relaxed);
            uint64_t expected = fh;
            uint64_t desired = (((fh >> 32) + 1) << 32) | next;
            if (free_head_.compare_exchange_weak(expected, desired,
                        std::memory_order_release, std::memory_order_acquire)) {
                size_.fetch_add(1, std::memory_order_relaxed);
                return &storage_[head];     // CAS 成功 → 取走头部
            }
            fh = expected;   // CAS 失败, 重读栈头重试(acquire 失败序)
        }
        return nullptr;                     // 空池
    }

    void deallocate(uint32_t idx)
    {
        uint64_t fh = free_head_.load(std::memory_order_acquire);
        for (;;) {
            uint32_t head = (uint32_t)fh;
            storage_[idx].pool_next_free.store(head, std::memory_order_relaxed);
            uint64_t expected = fh;
            uint64_t desired = (((fh >> 32) + 1) << 32) | idx;
            if (free_head_.compare_exchange_weak(expected, desired,
                        std::memory_order_release, std::memory_order_acquire)) {
                size_.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
            fh = expected;   // CAS 失败, 重读栈头重试
        }
    }

    void deallocate(Order* ptr)
    {
        if (!ptr) return;
        deallocate(static_cast<uint32_t>(ptr - storage_));
    }

    uint32_t indexOf(const Order* ptr) const
    {
        return static_cast<uint32_t>(ptr - storage_);
    }

    Order* at(uint32_t idx) { return &storage_[idx]; }
    const Order* at(uint32_t idx) const { return &storage_[idx]; }
    size_t capacity() const { return capacity_; }
    size_t size() const { return size_.load(std::memory_order_relaxed); }

    // 从头扫描 storage 重建空闲链表（崩溃恢复用）
    // 全部槽位加入 free list，addOrder 时重新分配
    void rebuildFreelist() {
        for (uint32_t i = 0; i < capacity_ - 1; i++)
            storage_[i].pool_next_free.store(i + 1, std::memory_order_relaxed);
        storage_[capacity_ - 1].pool_next_free.store(UINT32_MAX, std::memory_order_relaxed);
        free_head_.store(0, std::memory_order_release);
        size_.store(0, std::memory_order_relaxed);
    }

private:
    void initFreeList() {
        for (uint32_t i = 0; i < capacity_ - 1; ++i)
            storage_[i].pool_next_free.store(i + 1, std::memory_order_relaxed);
        storage_[capacity_ - 1].pool_next_free.store(UINT32_MAX, std::memory_order_relaxed);
        free_head_.store(0, std::memory_order_release);
        size_.store(0, std::memory_order_relaxed);
    }

    Order* const storage_;
    const size_t capacity_;
    bool owns_storage_ = true;
    // Treiber 无锁栈头 (tag<<32)|idx, tag 消 ABA；size_ 统计用(原子, 并发安全)
    std::atomic<uint64_t> free_head_ = 0;
    std::atomic<size_t>   size_ = 0;
};
