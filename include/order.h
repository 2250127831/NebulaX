#pragma once

#include <atomic>
#include <cstdint>

enum class Side : uint8_t
{
    INVALID,
    BUY,
    SELL
};

enum class OrderStatus : uint8_t
{
    OPEN,               // 挂单中
    PARTIALLY_FILLED,   // 部分成交
    FILLED,             // 已完全成交
    CANCELLED           // 已撤单
};

struct Order
{
    Order() = default;
    // pool_next_free 是 atomic（OrderPool 无锁），隐式拷贝被删除 → 自定义逐字段拷贝。
    // pool_next_free 仅池空闲链表用，拷贝时按 relaxed load/store 传递。
    Order(const Order& o) { *this = o; }
    Order& operator=(const Order& o) {
        if (this != &o) {
            user_id       = o.user_id;
            order_id      = o.order_id;
            side          = o.side;
            price         = o.price;
            original_qty  = o.original_qty;
            remaining_qty = o.remaining_qty;
            filled_qty    = o.filled_qty;
            sequence      = o.sequence;
            status        = o.status;
            prev_idx      = o.prev_idx;
            next_idx      = o.next_idx;
            pool_next_free.store(o.pool_next_free.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
        }
        return *this;
    }

    uint64_t user_id = 0;

    uint64_t order_id = 0;

    Side side = Side::INVALID;

    // 价格统一放大 100 倍存储
    // 例如:
    // 101.25 -> 10125
    uint32_t price = 0;

    // 原始下单量
    uint32_t original_qty = 0;

    // 剩余未成交量
    uint32_t remaining_qty = 0;

    // 已成交量
    uint32_t filled_qty = 0;

    // 时间优先（FIFO）
    // 先简单用递增序号代替时间戳
    uint64_t sequence = 0;

    OrderStatus status = OrderStatus::OPEN;

    // ── intrusive linked list (pool-managed, see order_pool.h) ──
    uint32_t prev_idx = UINT32_MAX;   // prev order in same price level
    uint32_t next_idx = UINT32_MAX;   // next order in same price level
    // 池空闲链表链接(仅释放时有效)。原子: OrderPool::allocate 无锁读
    // storage_[head].pool_next_free, 并发 deallocate 写同节点 → 非原子是 C++ UB。
    std::atomic<uint32_t> pool_next_free = UINT32_MAX;
};
static_assert(sizeof(Order) == 64, "Order must be 64 bytes for cache line alignment");