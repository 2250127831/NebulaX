#pragma once

#include "order.h"
#include <cstdint>
#include <cstddef>
#include <map>

// 分离链接法哈希表，底层池化管理，零堆分配（构造时预分配）。
// 用于 order_id → Order* 映射，替代 std::unordered_map。
class OrderMap
{
    struct Node
    {
        uint64_t order_id;
        Order*   order;
        uint32_t next_idx;   // 链指针 / 空闲链表
    };

public:
    OrderMap(size_t capacity)
        : nodes_(new Node[capacity])
        , buckets_(new uint32_t[roundPow2(capacity)])
        , bucket_len_(new uint16_t[roundPow2(capacity)]())
        , bucket_mask_(roundPow2(capacity) - 1)
        , hash_shift_(64 - __builtin_ctz(bucket_mask_ + 1))
        , capacity_(capacity)
    {
        for (uint32_t i = 0; i < capacity_ - 1; ++i)
            nodes_[i].next_idx = i + 1;
        nodes_[capacity_ - 1].next_idx = UINT32_MAX;
        free_head_ = 0;

        for (uint32_t i = 0; i <= bucket_mask_; ++i)
            buckets_[i] = UINT32_MAX;
    }

    ~OrderMap()
    {
        delete[] nodes_;
        delete[] buckets_;
        delete[] bucket_len_;
    }

    OrderMap(const OrderMap&) = delete;
    OrderMap& operator=(const OrderMap&) = delete;

    static constexpr uint32_t TREEIFY_THRESHOLD = 8;

    void insert(uint64_t order_id, Order* order)
    {
        uint32_t b = hash(order_id);
        if (bucket_len_[b] >= TREEIFY_THRESHOLD || free_head_ == UINT32_MAX) {
            overflow_[order_id] = order;
            ++size_;
            return;
        }

        uint32_t idx = allocNode();
        nodes_[idx].order_id = order_id;
        nodes_[idx].order    = order;
        nodes_[idx].next_idx = buckets_[b];
        buckets_[b] = idx;
        ++bucket_len_[b];
        ++size_;

        if (bucket_len_[b] >= TREEIFY_THRESHOLD) {
            // 将整个 bucket 移入 overflow map
            uint32_t cur = buckets_[b];
            while (cur != UINT32_MAX) {
                overflow_[nodes_[cur].order_id] = nodes_[cur].order;
                uint32_t next = nodes_[cur].next_idx;
                freeNode(cur);
                cur = next;
            }
            buckets_[b] = UINT32_MAX;
        }
    }

    Order* find(uint64_t order_id) const
    {
        uint32_t idx = buckets_[hash(order_id)];
        while (idx != UINT32_MAX) {
            if (nodes_[idx].order_id == order_id)
                return nodes_[idx].order;
            idx = nodes_[idx].next_idx;
        }
        auto it = overflow_.find(order_id);
        return (it != overflow_.end()) ? it->second : nullptr;
    }

    void erase(uint64_t order_id)
    {
        auto it = overflow_.find(order_id);
        if (it != overflow_.end()) {
            overflow_.erase(it);
            --size_;
            return;
        }

        uint32_t b = hash(order_id);
        uint32_t idx = buckets_[b];
        if (idx == UINT32_MAX) return;

        if (nodes_[idx].order_id == order_id) {
            buckets_[b] = nodes_[idx].next_idx;
            freeNode(idx);
            --bucket_len_[b];
            --size_;
            return;
        }

        while (nodes_[idx].next_idx != UINT32_MAX) {
            uint32_t next = nodes_[idx].next_idx;
            if (nodes_[next].order_id == order_id) {
                nodes_[idx].next_idx = nodes_[next].next_idx;
                freeNode(next);
                --bucket_len_[b];
                --size_;
                return;
            }
            idx = next;
        }
    }

    bool contains(uint64_t order_id) const
    {
        return find(order_id) != nullptr;
    }

    size_t size() const { return size_; }

private:
    Node* const    nodes_;
    uint32_t* const buckets_;
    uint16_t* const bucket_len_;    // 每个 bucket 的链长
    const uint32_t bucket_mask_;
    const uint32_t hash_shift_;
    const size_t   capacity_;
    uint32_t       free_head_ = UINT32_MAX;
    size_t         size_ = 0;
    std::map<uint64_t, Order*> overflow_;

    static uint32_t roundPow2(size_t n)
    {
        size_t p = 1;
        while (p < n) p <<= 1;
        return static_cast<uint32_t>(p);
    }

    uint32_t hash(uint64_t id) const
    {
        // 乘黄金常数取高位，对任何 ID 分布都均匀
        return (id * 0x9E3779B97F4A7C15ULL) >> hash_shift_;
    }

    uint32_t allocNode()
    {
        uint32_t idx = free_head_;
        free_head_ = nodes_[idx].next_idx;
        return idx;
    }

    void freeNode(uint32_t idx)
    {
        nodes_[idx].next_idx = free_head_;
        free_head_ = idx;
    }
};
