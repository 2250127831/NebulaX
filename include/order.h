#pragma once

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
};