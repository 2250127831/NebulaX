#pragma once

#include <iostream>
#include <map>
#include <list>
#include <unordered_map>
#include <functional>
#include <sstream>
#include "order.h"

// Top-of-book 数据（best bid / best ask 的聚合量）
struct TopOfBook
{
    uint32_t bid_price  = 0;
    uint32_t bid_volume = 0;
    uint32_t ask_price  = 0;
    uint32_t ask_volume = 0;
};

class OrderBook
{
public:
    using OrderList = std::list<Order>;

    struct OrderHandle
    {
        Side side;
        uint32_t price;

        OrderList::iterator it;
    };

public:
    OrderBook() = default;

    // 添加订单到盘口
    void addOrder(const Order& order);

    // 撤单
    bool removeOrder(uint64_t order_id, uint64_t user_id);

    // 获取最优买价（最高 bid）
    Order* getBestBid();

    // 获取最优卖价（最低 ask）
    Order* getBestAsk();

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
    std::map<
        uint32_t,
        OrderList,
        std::greater<>
    > bids_;

    // 卖盘（低价优先）
    std::map<
        uint32_t,
        OrderList
    > asks_;

    // order_id -> iterator
    std::unordered_map<
        uint64_t,
        OrderHandle
    > order_index_;
};