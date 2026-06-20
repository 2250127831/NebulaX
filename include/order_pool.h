#pragma once

#include "order.h"
#include <cstdint>
#include <cstddef>

// 固定容量 Order 池，构造时决定大小，空闲链表管理。
// 单块连续数组，Order* 永远稳定，at() 零开销。
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
        uint32_t idx = free_head_;
        if (idx == UINT32_MAX) return nullptr;
        free_head_ = storage_[idx].pool_next_free;
        ++size_;
        return &storage_[idx];
    }

    void deallocate(uint32_t idx)
    {
        storage_[idx].pool_next_free = free_head_;
        free_head_ = idx;
        --size_;
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
    size_t size() const { return size_; }

    // 从头扫描 storage 重建空闲链表（崩溃恢复用）
    // 全部槽位加入 free list，addOrder 时重新分配
    void rebuildFreelist() {
        for (uint32_t i = 0; i < capacity_ - 1; i++)
            storage_[i].pool_next_free = i + 1;
        storage_[capacity_ - 1].pool_next_free = UINT32_MAX;
        free_head_ = 0;
        size_ = 0;
    }

private:
    void initFreeList() {
        for (uint32_t i = 0; i < capacity_ - 1; ++i)
            storage_[i].pool_next_free = i + 1;
        storage_[capacity_ - 1].pool_next_free = UINT32_MAX;
        free_head_ = 0;
        size_ = 0;
    }

    Order* const storage_;
    const size_t capacity_;
    bool owns_storage_ = true;
    uint32_t free_head_ = 0;
    size_t size_ = 0;
};
