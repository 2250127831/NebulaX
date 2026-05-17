#include "order_book.h"
#include <iostream>
#include <inttypes.h>

// 添加订单到盘口
void OrderBook::addOrder(const Order& order)
{
    if (order.side == Side::INVALID) 
    {
        // 根据日志策略，直接返回或打 ERROR 日志
        return;
    }
    // 根据买卖方向选择盘口
    auto& priceLevel = (order.side == Side::BUY)
        ? bids_[order.price]
        : asks_[order.price];

    // 时间优先：追加到队尾
    priceLevel.push_back(order);

    // 记录迭代器，供撤单 O(1) 查找
    OrderHandle handle;
    handle.side = order.side;
    handle.price = order.price;
    handle.it = std::prev(priceLevel.end());

    order_index_[order.order_id] = handle;
}

// 撤单
bool OrderBook::removeOrder(uint64_t order_id, uint64_t user_id)
{
    auto it = order_index_.find(order_id);
    if (it == order_index_.end())
    {
        return false;
    }

    const OrderHandle& handle = it->second;

    if(handle.it->user_id != user_id)return false;

    // 根据方向从对应盘口删除
    if (handle.side == Side::BUY)
    {
        auto levelIt = bids_.find(handle.price);
        if (levelIt != bids_.end())
        {

            levelIt->second.erase(handle.it);

            // 如果该价格档位空了，清理掉
            if (levelIt->second.empty())
            {
                bids_.erase(levelIt);
            }
        }
    }
    else
    {
        auto levelIt = asks_.find(handle.price);
        if (levelIt != asks_.end())
        {
            levelIt->second.erase(handle.it);

            if (levelIt->second.empty())
            {
                asks_.erase(levelIt);
            }
        }
    }

    order_index_.erase(it);
    return true;
}

// 获取最优买价（最高 bid）
Order* OrderBook::getBestBid()
{
    if (bids_.empty())
    {
        return nullptr;
    }

    auto& level = bids_.begin()->second;
    if (level.empty())
    {
        return nullptr;
    }

    return &level.front();
}

// 获取最优卖价（最低 ask）
Order* OrderBook::getBestAsk()
{
    if (asks_.empty())
    {
        return nullptr;
    }

    auto& level = asks_.begin()->second;
    if (level.empty())
    {
        return nullptr;
    }

    return &level.front();
}

// 根据 order_id 查订单
Order* OrderBook::findOrder(uint64_t order_id)
{
    auto it = order_index_.find(order_id);
    if (it == order_index_.end())
    {
        return nullptr;
    }

    return &(*it->second.it);
}

// 打印前 N 档盘口
void OrderBook::printBook(int levels) const
{
    printf("\n==============BOOK_BEGIN==============\n");

    // 卖盘（asks_）从低价到高价，取前 levels 档
    int ask_count = 0;
    printf("\nASKS:\n");
    for (const auto& [price, order_list] : asks_) {
        if (ask_count >= levels) break;
        
        uint32_t total_vol = 0;
        for (const auto& order : order_list) {
            total_vol += order.remaining_qty;
        }
        printf("%d : price = %" PRIu32 "  ->  total = %" PRIu32 "\n",ask_count, price, total_vol);
        ++ask_count;
    }

    // 买盘（bids_）从高价到低价，取前 levels 档
    int bid_count = 0;
    printf("\nBIDS:\n");
    for (const auto& [price, order_list] : bids_) {
        if (bid_count >= levels) break;
        
        uint32_t total_vol = 0;
        for (const auto& order : order_list) {
            total_vol += order.remaining_qty;
        }
        printf("%d : price = %" PRIu32 "  ->  total = %" PRIu32 "\n",bid_count, price, total_vol);
        ++bid_count;
    }
    printf("\n==============BOOK_END==============\n");
}

// 获取前 N 档盘口
std::string OrderBook::getBookString(int levels) const
{
    std::ostringstream oss;

    oss << "\n==============BOOK_BEGIN==============\n";

    // 卖盘（asks_）从低价到高价，取前 levels 档
    int ask_count = 0;
    oss << "\nASKS:\n";
    for (const auto& [price, order_list] : asks_) {
        if (ask_count >= levels) break;

        uint32_t total_vol = 0;
        for (const auto& order : order_list) {
            total_vol += order.remaining_qty;
        }
        oss << ask_count << " : price = " << price
            << "  ->  total = " << total_vol << "\n";
        ++ask_count;
    }

    // 买盘（bids_）从高价到低价，取前 levels 档
    int bid_count = 0;
    oss << "\nBIDS:\n";
    for (const auto& [price, order_list] : bids_) {
        if (bid_count >= levels) break;

        uint32_t total_vol = 0;
        for (const auto& order : order_list) {
            total_vol += order.remaining_qty;
        }
        oss << bid_count << " : price = " << price
            << "  ->  total = " << total_vol << "\n";
        ++bid_count;
    }

    oss << "==============BOOK_END==============\n";
    return oss.str();
}