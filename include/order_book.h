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
    // 使用内部池（默认）
    explicit OrderBook(size_t pool_capacity = 4 << 20)
        : order_index_(pool_capacity)
        , owned_pool_(new OrderPool(pool_capacity))
        , pool_(owned_pool_.get())
    {}

    // 使用外部池（共享内存）
    explicit OrderBook(OrderPool* external_pool)
        : order_index_(external_pool->capacity())
        , pool_(external_pool)
    {}

    bool addOrder(const Order& order);
    bool removeOrder(uint64_t order_id, uint64_t user_id);
    void removeOrder(Order* order);
    Order* getBestBid(uint64_t exclude_user_id = 0);
    Order* getBestAsk(uint64_t exclude_user_id = 0);
    void reduceOrderQty(Order* order, uint32_t amount);
    TopOfBook getTopOfBook() const;
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
    OrderMap order_index_;
    std::unique_ptr<OrderPool> owned_pool_;  // 内部池（外部池时为 nullptr）
    OrderPool* pool_;                         // 始终指向可用池
};
