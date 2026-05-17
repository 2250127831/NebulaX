#include "matching_engine.h"
#include <sstream>
#include <iostream>

std::string MatchingEngine::processNewOrder(Side side, uint32_t price, uint32_t quantity, uint64_t user_id)
{
    // 拒绝无效买卖方向
    if (side == Side::INVALID)
    {
        return "ERROR invalid_side";
    }

    // 基础校验
    if (price == 0 || quantity == 0 || user_id == 0)
    {
        return "ERROR invalid_price_or_quantity_or_user";
    }

    // 构造订单对象
    Order order;
    order.user_id    = user_id;
    order.order_id   = next_order_id_++;
    order.side       = side;
    order.price      = price;
    order.original_qty = quantity;
    order.remaining_qty = quantity;
    order.filled_qty = 0;
    order.sequence   = next_sequence_++;
    order.status     = OrderStatus::OPEN;

    // 存放每笔成交字符串
    std::vector<std::string> trades;

    // 根据方向执行撮合
    if (side == Side::BUY)
    {
        matchBuyOrder(order, trades);
    }
    else // SELL
    {
        matchSellOrder(order, trades);
    }

    // 拼接返回结果
    std::ostringstream oss;

    // 先输出所有成交
    for (const auto& t : trades)
    {
        oss << t << "\n";
    }

    // 如果订单还有剩余，挂单并返回订单ID
    if (order.status == OrderStatus::OPEN ||
        order.status == OrderStatus::PARTIALLY_FILLED)
    {
        // 挂单
        order_book_.addOrder(order);
        oss << "OK order_id=" << order.order_id;
    }
    else if (order.status == OrderStatus::FILLED)
    {
        // 全部成交，不需要挂单
        oss << "FILLED order_id=" << order.order_id;
    }
    else if (order.status == OrderStatus::CANCELLED)
    {
        // 正常情况下不会走到这里
        oss << "CANCELLED order_id=" << order.order_id;
    }

    return oss.str();
}

std::string MatchingEngine::processCancel(uint64_t order_id, uint64_t user_id)
{
    // 尝试从订单簿删除
    bool removed = order_book_.removeOrder(order_id, user_id);

    if (removed)
    {
        return "CANCELLED " + std::to_string(order_id);
    }
    else
    {
        // 可能已经成交或不存在
        // 简单返回未找到（后续可扩展查历史成交）
        return "ERROR order_not_found";
    }
}

std::string MatchingEngine::getBook(int levels) const
{
    return order_book_.getBookString(levels);
}

void MatchingEngine::matchBuyOrder(Order& order, std::vector<std::string>& trades)
{
    // 只要还有剩余数量，并且卖盘最低价 <= 买价
    while (order.remaining_qty > 0)
    {
        Order* best_ask = order_book_.getBestAsk();
        if (!best_ask)
        {
            // 卖盘为空，停止撮合
            break;
        }

        if (order.price < best_ask->price)
        {
            // 买价低于最低卖价，无法成交
            break;
        }

        // 计算本次成交量
        uint32_t trade_qty = std::min(order.remaining_qty, best_ask->remaining_qty);

        // 更新双方数量
        order.remaining_qty -= trade_qty;
        order.filled_qty   += trade_qty;

        best_ask->remaining_qty -= trade_qty;
        best_ask->filled_qty   += trade_qty;

        // 记录成交记录
        std::ostringstream trade_msg;
        trade_msg << "TRADE " << best_ask->price << " " << trade_qty << " " << order.user_id << " " << best_ask->user_id << " " << order.order_id << " " << best_ask->order_id;//TRADE 成交价格 成交数量 买家id 卖家id 买入订单id 卖出订单id
        trades.push_back(trade_msg.str());

        // 如果卖单已完全成交，从订单簿移除
        if (best_ask->remaining_qty == 0)
        {
            best_ask->status = OrderStatus::FILLED;
            order_book_.removeOrder(best_ask->order_id, best_ask->user_id);
        }
        else
        {
            best_ask->status = OrderStatus::PARTIALLY_FILLED;
        }

        // 更新当前买单状态
        if (order.remaining_qty == 0)
        {
            order.status = OrderStatus::FILLED;
        }
        else
        {
            order.status = OrderStatus::PARTIALLY_FILLED;
        }
    }
}

void MatchingEngine::matchSellOrder(Order& order, std::vector<std::string>& trades)
{
    // 只要还有剩余数量，并且买盘最高价 >= 卖价
    while (order.remaining_qty > 0)
    {
        Order* best_bid = order_book_.getBestBid();
        if (!best_bid)
        {
            // 买盘为空，停止撮合
            break;
        }

        if (order.price > best_bid->price)
        {
            // 卖价高于最高买价，无法成交
            break;
        }

        // 计算本次成交量
        uint32_t trade_qty = std::min(order.remaining_qty, best_bid->remaining_qty);

        // 更新双方数量
        order.remaining_qty -= trade_qty;
        order.filled_qty   += trade_qty;

        best_bid->remaining_qty -= trade_qty;
        best_bid->filled_qty   += trade_qty;

        // 记录成交
        std::ostringstream trade_msg;
        trade_msg << "TRADE " << best_bid->price << " " << trade_qty << " " << best_bid->user_id << " " << order.user_id << " " << best_bid->order_id << " " << order.order_id;//TRADE 成交价格 成交数量 买家id 卖家id 买入订单id 卖出订单id
        trades.push_back(trade_msg.str());

        // 如果买单已完全成交，从订单簿移除
        if (best_bid->remaining_qty == 0)
        {
            best_bid->status = OrderStatus::FILLED;
            order_book_.removeOrder(best_bid->order_id,best_bid->user_id);
        }
        else
        {
            best_bid->status = OrderStatus::PARTIALLY_FILLED;
        }

        // 更新当前卖单状态
        if (order.remaining_qty == 0)
        {
            order.status = OrderStatus::FILLED;
        }
        else
        {
            order.status = OrderStatus::PARTIALLY_FILLED;
        }
    }
}