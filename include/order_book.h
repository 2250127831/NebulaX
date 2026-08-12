#pragma once

#include <iostream>
#include <map>
#include <functional>
#include <sstream>
#include <memory>
#include "order.h"
#include "order_pool.h"
#include "order_map.h"

struct TopOfBook
{
    uint32_t bid_price  = 0;
    uint32_t bid_volume = 0;
    uint32_t ask_price  = 0;
    uint32_t ask_volume = 0;
};

struct PriceLevel
{
    uint32_t head_idx = UINT32_MAX;
    uint32_t tail_idx = UINT32_MAX;
    uint32_t count = 0;
    uint32_t total_qty = 0;
};

class OrderBook
{
public:
    // 使用内部池 + 内部索引（默认，单簿自建）
    explicit OrderBook(size_t pool_capacity = 4 << 20)
        : owned_pool_(new OrderPool(pool_capacity))
        , owned_index_(new OrderMap(pool_capacity))
        , pool_(owned_pool_.get())
        , index_(owned_index_.get())
    {}

    // 使用外部池（共享内存）+ 内部索引（单簿）
    explicit OrderBook(OrderPool* external_pool)
        : owned_index_(new OrderMap(external_pool->capacity()))
        , pool_(external_pool)
        , index_(owned_index_.get())
    {}

    // 共享池 + 共享索引（分簿模式，多簿共用一份，迁移自 NebulaX-Trader）。
    // 多标的下每簿只保留盘口(bids_/asks_)，挂单数据与索引全局唯一——杜绝每簿
    // 自建池/索引的 OOM。OrderPool/OrderMap 由外部(主线程)创建并共享。
    OrderBook(OrderPool& shared_pool, OrderMap& shared_index)
        : pool_(&shared_pool)
        , index_(&shared_index)
    {}

    bool addOrder(const Order& order);
    bool removeOrder(uint64_t order_id, uint64_t user_id);
    void removeOrder(Order* order);
    Order* getBestBid(uint64_t exclude_user_id = 0);
    Order* getBestAsk(uint64_t exclude_user_id = 0);
    void reduceOrderQty(Order* order, uint32_t amount);
    TopOfBook getTopOfBook() const;

    // 对手盘可吃总量（FOK 预检查用）：按价格-时间优先累计可成交量。
    // side = 我方方向；price = 我方价格（市价单传极端值）。
    // 返回对手盘在价格范围内的可吃总余量。
    uint32_t availableQty(Side side, uint32_t price) const;
    Order* findOrder(uint64_t order_id);
    size_t poolUsage() const { return pool_->size(); }
    size_t poolCapacity() const { return pool_->capacity(); }
    uint64_t saveSnapshot(const char* path) const;
    void loadSnapshot(const char* path, uint64_t& max_seq_out, uint64_t& max_id_out);
    std::string getBookString(int levels) const;
    void printBook(int levels) const;

    OrderPool& getPool() { return *pool_; }  // recoverFromShared 需要

private:
    std::map<uint32_t, PriceLevel, std::greater<>> bids_;
    std::map<uint32_t, PriceLevel> asks_;
    std::unique_ptr<OrderPool> owned_pool_;  // 内部池（外部池/共享池时为 nullptr）
    std::unique_ptr<OrderMap> owned_index_;  // 内部索引（共享索引时为 nullptr）
    OrderPool* pool_;                         // 始终指向可用池
    OrderMap*  index_;                        // 始终指向可用索引
};
