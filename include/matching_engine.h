#pragma once

#include <atomic>
#include <vector>
#include <memory>

#include "order_book.h"
#include "protocol.h"
#include "metrics.h"

class MatchingEngine
{
public:
    // ITCH Stock Locate 是 16-bit(0-65535)。books_ 用固定数组 + 原子指针：
    // 并发首次建簿 CAS，之后无锁读；无 unordered_map 的 rehash 指针失效。
    //
    // 线程安全契约（回测平台多线程驱动）：
    //   - 不同 locate（symbol）的订单簿可并发处理（不同线程驱动不同 symbol）
    //   - 同一 locate 的操作必须串行（单线程或外部锁）——撮合读写簿非线程安全
    //   - 共享 OrderPool/OrderMap 无锁，跨簿并发操作安全（槽复用经 acquire/release 配对）
    static constexpr uint32_t kMaxLocate = 65536;

    // ── 分簿模式：外部共享 OrderPool + OrderMap（多簿共用，回测/多标的）──
    MatchingEngine(OrderPool& shared_pool, OrderMap& shared_index, IOCounters* metrics = nullptr);

    // ── 单簿兼容：自建池 + 索引 ──
    explicit MatchingEngine(size_t pool_capacity, IOCounters* metrics = nullptr);

    // ── 单簿兼容：外部池（共享内存）+ 自建索引 ──
    explicit MatchingEngine(OrderPool* external_pool, IOCounters* metrics = nullptr);

    // 取指定 locate 的订单簿（不存在则惰性建簿，并发安全）。locate 越界返回 nullptr。
    OrderBook* book_for(uint64_t locate);

    // ── ITCH 语义 API（分簿）──
    // A/F (Add Order): 新订单，order_ref 外部提供（全局唯一）
    // tif=DAY: 限价单，剩余挂簿（默认）。price 必填。
    // tif=IOC: 市价单，立即成交，剩余作废不挂簿；无对手盘拒绝（MKT_NO_LIQUIDITY）。
    //   price 被忽略（市场价 = 对手盘价）。
    // tif=FOK: 必须全部成交，否则整个作废（FOK_NO_FULL_FILL）。可配 price（限价 FOK）。
    void processAdd(uint64_t locate, uint64_t order_ref, Side side,
                    uint32_t price, uint32_t quantity, uint64_t user_id,
                    std::vector<BinaryResponse>& out_responses,
                    OrderTif tif = OrderTif::DAY);
    // D (Order Delete): 整笔撤单
    void processCancel(uint64_t locate, uint64_t order_ref, uint64_t user_id,
                       std::vector<BinaryResponse>& out_responses);
    // X (Order Cancel): 部分撤单
    void processCancelShares(uint64_t locate, uint64_t order_ref, uint32_t shares,
                             uint64_t user_id, std::vector<BinaryResponse>& out_responses);
    // U (Order Replace): 改单，old_ref 作废，new_ref 以新价/新量挂出
    void processReplace(uint64_t locate, uint64_t old_ref, uint64_t new_ref, Side side,
                        uint32_t price, uint32_t quantity, uint64_t user_id,
                        std::vector<BinaryResponse>& out_responses);

    // 返回指定 locate 的 top of book（best bid / best ask）
    void getBook(uint64_t locate, BinaryResponse& out_response) const;

    // ── 单簿兼容 API（路由到 locate=0，P5 前 TcpServer 沿用）──
    void processNewOrder(Side side, uint32_t price, uint32_t quantity, uint64_t user_id,
                         std::vector<BinaryResponse>& out_responses);
    void processCancel(uint64_t order_id, uint64_t user_id,
                       std::vector<BinaryResponse>& out_responses);

    // 停机快照：将所有 resting orders 写入文件
    void saveSnapshot(const char* path) const;

    // 加载快照：从文件恢复订单簿
    void loadSnapshot(const char* path);

private:
    // 买单撮合逻辑（locate 指定簿）
    void matchBuyOrder(uint64_t locate, OrderBook& book, Order& order, std::vector<BinaryResponse>& out);
    // 卖单撮合逻辑
    void matchSellOrder(uint64_t locate, OrderBook& book, Order& order, std::vector<BinaryResponse>& out);
    // FOK 预检查：对手盘能否全成交 quantity（按价格-时间优先扫描可吃量）
    bool canFullFill(const OrderBook& book, Side side, uint32_t price, uint32_t quantity) const;

private:
    // locate → 独立簿。惰性建簿，固定数组原子指针（回测多线程并发驱动不同 symbol）。
    std::unique_ptr<std::atomic<OrderBook*>[]> books_;
    OrderPool* shared_pool_ = nullptr;   // 共享池（外部或自建 owned_）
    OrderMap*  shared_index_ = nullptr;  // 共享索引（外部或自建 owned_）
    std::unique_ptr<OrderPool> owned_pool_;   // 单簿兼容时自建
    std::unique_ptr<OrderMap>  owned_index_;  // 单簿兼容时自建

    std::atomic<uint64_t> next_order_id_{1};
    std::atomic<uint64_t> next_sequence_{1};

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
