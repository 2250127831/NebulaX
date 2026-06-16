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
    uint32_t total_qty = 0;
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

    // 撮合时减少盘口订单余量，自动维护 PriceLevel 总余量
    // trade_qty = std::min(remaining_qty, best_ask->remaining_qty)
    void reduceOrderQty(Order* order, uint32_t amount);

    // 获取 top-of-book（best bid / best ask 的聚合量）
    TopOfBook getTopOfBook() const;

    // 根据 order_id 查订单
    Order* findOrder(uint64_t order_id);

    // 返回 OrderPool 已用量（监控用）
    size_t poolUsage() const { return pool_.size(); }
    size_t poolCapacity() const { return pool_.capacity(); }

    // 停机快照：将所有 resting orders 写入文件（路径固定为 /tmp/nebulaX_snapshot.dat）
    // 返回 max_order_id（恢复后应从此 +1 继续分配）
    uint64_t saveSnapshot(const char* path) const;

    // 加载快照：从文件恢复订单簿（返回 max_order_id）
    // max_seq_out 和 max_id_out 用加载后应分配的起始值
    void loadSnapshot(const char* path, uint64_t& max_seq_out, uint64_t& max_id_out);

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