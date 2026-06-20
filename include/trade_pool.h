#pragma once

#include <cstdint>
#include <atomic>

static constexpr size_t TRADE_CAPACITY = 1 << 20;  // 约 100 万笔

struct TradeEntry {
    uint64_t trade_id;
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    uint32_t price;
    uint32_t quantity;
    uint64_t buyer_id;
    uint64_t seller_id;
    uint64_t timestamp;
    uint64_t seq;           // 对应 WAL seq
};

// 近期成交环形缓冲，满覆盖旧数据。写入共享内存。
struct TradePool {
    TradeEntry entries[TRADE_CAPACITY]{};
    std::atomic<uint64_t> write_idx{0};
};
