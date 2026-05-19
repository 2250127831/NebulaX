#pragma once

#include <vector>

#include "order_book.h"
#include "protocol.h"

class MatchingEngine
{
public:
    MatchingEngine() = default;

    // 处理新订单
    // 往 out_responses 追加 TRADE + 最终状态
    void processNewOrder(
        Side side,
        uint32_t price,
        uint32_t quantity,
        uint64_t user_id,
        std::vector<BinaryResponse>& out_responses
    );

    // 处理撤单
    void processCancel(
        uint64_t order_id,
        uint64_t user_id,
        std::vector<BinaryResponse>& out_responses
    );

    // 返回 top of book（best bid / best ask）
    void getBook(
        BinaryResponse& out_response
    ) const;

private:
    // 买单撮合逻辑
    // 往 out 追加 TRADE 记录
    void matchBuyOrder(
        Order& order,
        std::vector<BinaryResponse>& out
    );

    // 卖单撮合逻辑
    void matchSellOrder(
        Order& order,
        std::vector<BinaryResponse>& out
    );

private:
    OrderBook order_book_;

    // 全局订单 id，每次下单自增
    uint64_t next_order_id_ = 1;

    // 全局顺序号，用于时间优先（FIFO）
    uint64_t next_sequence_ = 1;
};
