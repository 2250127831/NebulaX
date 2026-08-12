#pragma once

#include "order.h"
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <map>
#include <mutex>

// ── order_id → Order* 哈希表（迁移自 NebulaX-Trader order_map.h）──
// 分离链接法，底层池化管理，零堆分配（构造时预分配），替代 std::unordered_map。
// 桶过长(≥8)时整体迁移到 std::map overflow（TREEIFY，防退化）。
//
// 并发设计（见 docs/ORDER_MAP_CONCURRENCY.md）：
//   Node.bucket 是并发安全的根基——节点被归还池后可能被复用(内存池是栈, 立即复用),
//   next 被改写指向别的桶链 → 无锁 find 可能被带偏。find 每步校验 bucket==目标桶,
//   不匹配 = 链被并发跳走 → 重试，用数据结构规避节点复用导致的 ABA(跳链)，
//   不需要惰性删除/hazard pointer/延迟回收。
class OrderMap
{
    struct Node
    {
        std::atomic<uint64_t> order_id;   // 原子: find 无锁读 vs insert 写(节点复用)
        std::atomic<Order*>   order;      // 同上
        std::atomic<uint32_t> next_idx;   // 链指针 / 空闲链表(共用, 摘除 CAS 用)
        std::atomic<uint32_t> bucket;     // 本节点所属桶(防 find 跳链, 原子消除读写竞争)
    };

public:
    OrderMap(size_t capacity)
        : nodes_(new Node[capacity])
        , buckets_(new std::atomic<uint32_t>[roundPow2(capacity)])
        , bucket_len_(new std::atomic<uint16_t>[roundPow2(capacity)])
        , state_(new std::atomic<uint8_t>[roundPow2(capacity)])
        , bucket_mask_(roundPow2(capacity) - 1)
        , hash_shift_(64 - __builtin_ctz(bucket_mask_ + 1))
        , capacity_(capacity)
    {
        for (uint32_t i = 0; i < capacity_ - 1; ++i)
            nodes_[i].next_idx.store(i + 1, std::memory_order_relaxed);
        nodes_[capacity_ - 1].next_idx.store(UINT32_MAX, std::memory_order_relaxed);
        free_head_.store(0, std::memory_order_release);

        for (uint32_t i = 0; i <= bucket_mask_; ++i) {
            buckets_[i].store(UINT32_MAX, std::memory_order_relaxed);
            bucket_len_[i].store(0, std::memory_order_relaxed);
            state_[i].store(0, std::memory_order_relaxed);
        }
    }

    ~OrderMap()
    {
        delete[] nodes_;
        delete[] buckets_;
        delete[] bucket_len_;
        delete[] state_;
    }

    OrderMap(const OrderMap&) = delete;
    OrderMap& operator=(const OrderMap&) = delete;

    static constexpr uint32_t TREEIFY_THRESHOLD = 8;

    // 返回是否插入成功（false = 节点池耗尽，丢弃）。
    bool insert(uint64_t order_id, Order* order)
    {
        uint32_t b = hash(order_id);
        // 状态门禁: 链路径每一步前检查 state_。读到 ≠0(转化中/已树化) → 锁 overflow_ 写。
        if (state_[b].load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(overflow_lock_);
            overflow_[order_id] = order;
            size_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // 桶长≥阈值 或 空闲满 → 触发转化(0→1 CAS)。
        if (bucket_len_[b].load(std::memory_order_relaxed) >= TREEIFY_THRESHOLD ||
            free_head_.load(std::memory_order_relaxed) == UINT32_MAX) {
            treeify(b);
            std::lock_guard<std::mutex> lk(overflow_lock_);
            overflow_[order_id] = order;
            size_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        uint32_t idx = allocNode();
        if (idx == UINT32_MAX) return false;   // 节点池耗尽, 丢弃(不越界写)
        nodes_[idx].order_id.store(order_id, std::memory_order_relaxed);
        nodes_[idx].order.store(order, std::memory_order_relaxed);
        nodes_[idx].bucket.store(b, std::memory_order_relaxed);
        // 链头 CAS 前重查状态: treeify 可能在 allocNode 期间启动(0→1), 读到则重试进 map。
        if (state_[b].load(std::memory_order_acquire)) {
            freeNode(idx);
            std::lock_guard<std::mutex> lk(overflow_lock_);
            overflow_[order_id] = order;
            size_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        uint32_t head = buckets_[b].load(std::memory_order_relaxed);
        nodes_[idx].next_idx.store(head, std::memory_order_relaxed);
        while (!buckets_[b].compare_exchange_weak(head, idx,
                    std::memory_order_release, std::memory_order_relaxed)) {
            if (state_[b].load(std::memory_order_acquire)) {
                freeNode(idx);
                std::lock_guard<std::mutex> lk(overflow_lock_);
                overflow_[order_id] = order;
                size_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            nodes_[idx].next_idx.store(head, std::memory_order_relaxed);
        }
        bucket_len_[b].fetch_add(1, std::memory_order_relaxed);
        size_.fetch_add(1, std::memory_order_relaxed);

        if (bucket_len_[b].load(std::memory_order_relaxed) >= TREEIFY_THRESHOLD)
            treeify(b);
        return true;
    }

    Order* find(uint64_t order_id) const
    {
        uint32_t b = hash(order_id);
        if (state_[b].load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(overflow_lock_);
            auto it = overflow_.find(order_id);
            return (it != overflow_.end()) ? it->second : nullptr;
        }
        // 无锁读链 + bucket 校验 + 重试: 读到别的桶的节点 = 链被并发跳走 → 重来,
        // 而不是返回 not found (k 可能还在, 只是链刚被并发改过)。
    retry:
        uint32_t idx = buckets_[b].load(std::memory_order_acquire);
        while (idx != UINT32_MAX) {
            if (nodes_[idx].bucket.load(std::memory_order_relaxed) != b) goto retry;
            if (nodes_[idx].order_id.load(std::memory_order_relaxed) == order_id)
                return nodes_[idx].order.load(std::memory_order_relaxed);
            idx = nodes_[idx].next_idx.load(std::memory_order_relaxed);
        }
        return nullptr;
    }

    void erase(uint64_t order_id)
    {
        uint32_t b = hash(order_id);
        if (state_[b].load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(overflow_lock_);
            auto it = overflow_.find(order_id);
            if (it != overflow_.end()) {
                overflow_.erase(it);
                size_.fetch_sub(1, std::memory_order_relaxed);
            }
            return;
        }

    retry:
        uint32_t idx = buckets_[b].load(std::memory_order_acquire);
        if (idx == UINT32_MAX) return;

        if (nodes_[idx].order_id.load(std::memory_order_relaxed) == order_id) {   // 链头
            uint32_t nxt = nodes_[idx].next_idx.load(std::memory_order_relaxed);
            while (!buckets_[b].compare_exchange_weak(idx, nxt,
                        std::memory_order_release, std::memory_order_acquire)) {
                if (nodes_[idx].order_id.load(std::memory_order_relaxed) != order_id) goto retry;
                nxt = nodes_[idx].next_idx.load(std::memory_order_relaxed);
            }
            freeNode(idx);
            bucket_len_[b].fetch_sub(1, std::memory_order_relaxed);
            size_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }

        // 链中删除: 找前驱, CAS 前驱的 next 跳过本节点。
        // 前驱可能是别人的节点, 会被其 owner 并发 erase → freeNode 复用。bucket 校验:
        //   读到 bucket≠b 的前驱 = 已被复用跳桶 → 重试。CAS 返回值是唯一权威。
        while (idx != UINT32_MAX) {
            if (nodes_[idx].bucket.load(std::memory_order_relaxed) != b) goto retry;
            uint32_t next = nodes_[idx].next_idx.load(std::memory_order_relaxed);
            if (next == UINT32_MAX) return;
            if (nodes_[next].order_id.load(std::memory_order_relaxed) == order_id) {
                uint32_t nxt2 = nodes_[next].next_idx.load(std::memory_order_relaxed);
                if (nodes_[idx].next_idx.compare_exchange_weak(
                        next, nxt2, std::memory_order_release, std::memory_order_relaxed)) {
                    freeNode(next);
                    bucket_len_[b].fetch_sub(1, std::memory_order_relaxed);
                    size_.fetch_sub(1, std::memory_order_relaxed);
                    return;
                }
                goto retry;
            }
            idx = next;
        }
    }

    bool contains(uint64_t order_id) const
    {
        return find(order_id) != nullptr;
    }

    size_t size() const { return size_.load(std::memory_order_relaxed); }

private:
    Node* const    nodes_;
    std::atomic<uint32_t>* const buckets_;     // 链头(无锁 CAS)
    std::atomic<uint16_t>* const bucket_len_;  // 每桶链长(原子, 树化阈值判断, 允许近似)
    // 每桶转化状态(原子, 树化门禁): 0=链(可无锁操作), 1=转化中, 2=已树化(锁 overflow_ 走 map)
    std::atomic<uint8_t>* const state_;
    const uint32_t bucket_mask_;
    const uint32_t hash_shift_;
    const size_t   capacity_;
    // Treiber 无锁空闲栈, 带 tag(高32位)消除 ABA: 栈头存 (tag<<32)|idx, 每次成功
    // alloc/free 都 tag+1。节点归还后立即复用, 并发 alloc/free 时若无 tag, 一个线程
    // 读 (head=X,next=Y) 后另一线程 alloc+free X(X回栈顶), 前者的 CAS(X→Y) 仍成功
    // 但 Y 已非 X 的 next → 两个线程同时"拥有"X → 链上节点被覆盖。tag 保证 stale CAS 失败。
    std::atomic<uint64_t> free_head_ = UINT32_MAX;
    std::atomic<size_t>   size_ = 0;
    mutable std::mutex overflow_lock_;
    mutable std::map<uint64_t, Order*> overflow_;

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
        uint64_t fh = free_head_.load(std::memory_order_acquire);
        while ((uint32_t)fh != UINT32_MAX) {
            uint32_t head = (uint32_t)fh;
            uint32_t next = nodes_[head].next_idx.load(std::memory_order_relaxed);
            uint64_t expected = fh;
            uint64_t desired = (((fh >> 32) + 1) << 32) | next;
            if (free_head_.compare_exchange_weak(expected, desired,
                        std::memory_order_release, std::memory_order_acquire))
                return head;
            fh = expected;
        }
        return UINT32_MAX;
    }

    void freeNode(uint32_t idx)
    {
        uint64_t fh = free_head_.load(std::memory_order_acquire);
        for (;;) {
            uint32_t head = (uint32_t)fh;
            nodes_[idx].next_idx.store(head, std::memory_order_relaxed);
            uint64_t expected = fh;
            uint64_t desired = (((fh >> 32) + 1) << 32) | idx;
            if (free_head_.compare_exchange_weak(expected, desired,
                        std::memory_order_release, std::memory_order_acquire))
                return;
            fh = expected;
        }
    }

    // 桶树化(桶长≥8): CAS 0→1 抢占转化权(防并发触发), 锁 overflow_lock_ 复制式迁移
    // 链节点进 overflow_ → 置 2(已树化) → 解锁。转化期间链操作读到 state_=1 会重试进 map。
    void treeify(uint32_t b)
    {
        uint8_t expect = 0;
        if (!state_[b].compare_exchange_strong(expect, 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;   // 已被并发转化或已树化
        }
        std::lock_guard<std::mutex> lk(overflow_lock_);
        uint32_t cur = buckets_[b].load(std::memory_order_relaxed);
        while (cur != UINT32_MAX) {
            overflow_[nodes_[cur].order_id.load(std::memory_order_relaxed)] =
                nodes_[cur].order.load(std::memory_order_relaxed);
            cur = nodes_[cur].next_idx.load(std::memory_order_relaxed);
        }
        state_[b].store(2, std::memory_order_release);
    }
};
