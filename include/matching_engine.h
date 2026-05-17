#pragma once

#include <string>
#include <vector>

#include "order_book.h"

class MatchingEngine
{
public:
    MatchingEngine() = default;

    // 处理新订单
    // 返回执行结果字符串
    std::string processNewOrder(
        Side side,
        uint32_t price,
        uint32_t quantity,
        uint64_t user_id
    );

    // 处理撤单
    std::string processCancel(
        uint64_t order_id,
        uint64_t user_id
    );

    // 返回盘口信息
    std::string getBook(
        int levels = 5
    ) const;

private:
    // 买单撮合逻辑
    void matchBuyOrder(
        Order& order,

        // 存放成交结果
        std::vector<std::string>& trades
    );

    // 卖单撮合逻辑
    void matchSellOrder(
        Order& order,

        // 存放成交结果
        std::vector<std::string>& trades
    );

private:
    // 核心订单簿
    // 保存 bid / ask
    OrderBook order_book_;

    // 全局订单 id
    // 每次下单自增
    uint64_t next_order_id_ = 1;

    // 全局顺序号
    // 用于时间优先（FIFO）
    uint64_t next_sequence_ = 1;
};