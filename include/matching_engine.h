#pragma once

#include <vector>

#include "order_book.h"
#include "protocol.h"
#include "metrics.h"

class MatchingEngine
{
public:
    explicit MatchingEngine(IOCounters* metrics = nullptr)
        : order_book_(), metrics_(metrics)
    {
        if (metrics_) metrics_->order_pool_capacity = order_book_.poolCapacity();
    }

    // 使用外部 OrderPool（共享内存）
    explicit MatchingEngine(OrderPool* external_pool, IOCounters* metrics = nullptr)
        : order_book_(external_pool), metrics_(metrics)
    {
        if (metrics_) metrics_->order_pool_capacity = order_book_.poolCapacity();
    }

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

    // 停机快照：将所有 resting orders 写入文件
    void saveSnapshot(const char* path) const;

    // 加载快照：从文件恢复订单簿
    // 必须在处理任何命令之前调用
    void loadSnapshot(const char* path);

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

    uint64_t next_order_id_ = 1;
    uint64_t next_sequence_ = 1;

    mutable IOCounters* metrics_ = nullptr;

public:
    // 从共享内存恢复 OrderPool → 重建 bids_/asks_
    void recoverFromShared(Order* order_storage, size_t capacity);

    // 从 WAL 文件恢复：打开 WAL，逐条幂等回放
    void recoverFromWal(const char* wal_path);

    // WAL / TradePool 指针（由 main.cpp 设置）
    class WalWriter* wal_ = nullptr;
    class TradePool* trade_pool_ = nullptr;
    uint8_t*        book_base_ = nullptr;   // 共享内存基址
    size_t          book_size_ = 0;         // 共享内存大小

    // WAL 满时 fork checkpoint
    void checkpointIfNeeded();

};
