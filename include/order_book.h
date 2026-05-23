#pragma once

#include <iostream>
#include <map>
#include <functional>
#include <sstream>
#include "order.h"
#include "order_pool.h"
#include "order_map.h"

// Top-of-book 数据（best bid / best ask 的聚合量）
struct TopOfBook
{
    uint32_t bid_price  = 0;
    uint32_t bid_volume = 0;
    uint32_t ask_price  = 0;
    uint32_t ask_volume = 0;
};

// 价格档元数据（池索引构成双向链表）
struct PriceLevel
{
    uint32_t head_idx = UINT32_MAX;
    uint32_t tail_idx = UINT32_MAX;
    uint32_t count = 0;
};

class OrderBook
{
public:
    explicit OrderBook(size_t pool_capacity = 4 << 20)
        : order_index_(pool_capacity)
        , pool_(pool_capacity)
    {
    }

    // 添加订单到盘口（返回 false 表示池满）
    bool addOrder(const Order& order);

    // 撤单
    bool removeOrder(uint64_t order_id, uint64_t user_id);
    void removeOrder(Order* order);

    // 获取最优买价（最高 bid），可排除指定 user_id（自成交防护）
    Order* getBestBid(uint64_t exclude_user_id = 0);

    // 获取最优卖价（最低 ask），可排除指定 user_id（自成交防护）
    Order* getBestAsk(uint64_t exclude_user_id = 0);

    // 获取 top-of-book（best bid / best ask 的聚合量）
    TopOfBook getTopOfBook() const;

    // 根据 order_id 查订单
    Order* findOrder(uint64_t order_id);

    // 获取前 N 档盘口
    std::string getBookString(int levels) const;

    //打印前 N 档盘口
    void printBook(int levels) const;

private:
    // 买盘（高价优先）
    std::map<uint32_t, PriceLevel, std::greater<>> bids_;

    // 卖盘（低价优先）
    std::map<uint32_t, PriceLevel> asks_;

    // order_id → Order*
    OrderMap order_index_;

    OrderPool pool_;
};