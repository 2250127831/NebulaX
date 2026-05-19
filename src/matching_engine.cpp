#include "matching_engine.h"
#include <algorithm>
#include <cstdint>

void MatchingEngine::processNewOrder(
    Side side,
    uint32_t price,
    uint32_t quantity,
    uint64_t user_id,
    std::vector<BinaryResponse>& out)
{
    if (side == Side::INVALID)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_SIDE);
        return;
    }

    if (price == 0 || quantity == 0 || user_id == 0)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_PRICE_QTY_USER);
        return;
    }

    Order order;
    order.user_id       = user_id;
    order.order_id      = next_order_id_++;
    order.side          = side;
    order.price         = price;
    order.original_qty  = quantity;
    order.remaining_qty = quantity;
    order.filled_qty    = 0;
    order.sequence      = next_sequence_++;
    order.status        = OrderStatus::OPEN;

    if (side == Side::BUY)
        matchBuyOrder(order, out);
    else
        matchSellOrder(order, out);

    if (order.status == OrderStatus::OPEN ||
        order.status == OrderStatus::PARTIALLY_FILLED)
    {
        order_book_.addOrder(order);
        auto& rsp = out.emplace_back();
        rsp.type = RSP_OK;
        rsp.data.ack.order_id = order.order_id;
    }
    else if (order.status == OrderStatus::FILLED)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_FILLED;
        rsp.data.ack.order_id = order.order_id;
    }
}

void MatchingEngine::processCancel(
    uint64_t order_id,
    uint64_t user_id,
    std::vector<BinaryResponse>& out)
{
    bool removed = order_book_.removeOrder(order_id, user_id);

    auto& rsp = out.emplace_back();
    if (removed)
    {
        rsp.type = RSP_CANCELLED;
        rsp.data.ack.order_id = order_id;
    }
    else
    {
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::ORDER_NOT_FOUND);
    }
}

void MatchingEngine::getBook(BinaryResponse& out) const
{
    out.type = RSP_BOOK;

    TopOfBook tob = order_book_.getTopOfBook();
    out.data.book.bid_price  = tob.bid_price;
    out.data.book.bid_volume = tob.bid_volume;
    out.data.book.ask_price  = tob.ask_price;
    out.data.book.ask_volume = tob.ask_volume;
}

void MatchingEngine::matchBuyOrder(Order& order, std::vector<BinaryResponse>& out)
{
    while (order.remaining_qty > 0)
    {
        Order* best_ask = order_book_.getBestAsk();
        if (!best_ask) break;

        if (order.price < best_ask->price) break;

        uint32_t trade_qty = std::min(order.remaining_qty, best_ask->remaining_qty);

        order.remaining_qty -= trade_qty;
        order.filled_qty   += trade_qty;

        best_ask->remaining_qty -= trade_qty;
        best_ask->filled_qty   += trade_qty;

        // 记录成交
        {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_TRADE;
            rsp.data.trade.price         = best_ask->price;
            rsp.data.trade.quantity      = trade_qty;
            rsp.data.trade.buyer_id      = order.user_id;
            rsp.data.trade.seller_id     = best_ask->user_id;
            rsp.data.trade.buy_order_id  = order.order_id;
            rsp.data.trade.sell_order_id = best_ask->order_id;
        }

        if (best_ask->remaining_qty == 0)
        {
            best_ask->status = OrderStatus::FILLED;
            // removeOrder 后 best_ask 悬空，循环顶部重新获取
            order_book_.removeOrder(best_ask->order_id, best_ask->user_id);
        }
        else
        {
            best_ask->status = OrderStatus::PARTIALLY_FILLED;
        }

        order.status = (order.remaining_qty == 0)
            ? OrderStatus::FILLED
            : OrderStatus::PARTIALLY_FILLED;
    }
}

void MatchingEngine::matchSellOrder(Order& order, std::vector<BinaryResponse>& out)
{
    while (order.remaining_qty > 0)
    {
        Order* best_bid = order_book_.getBestBid();
        if (!best_bid) break;

        if (order.price > best_bid->price) break;

        uint32_t trade_qty = std::min(order.remaining_qty, best_bid->remaining_qty);

        order.remaining_qty -= trade_qty;
        order.filled_qty   += trade_qty;

        best_bid->remaining_qty -= trade_qty;
        best_bid->filled_qty   += trade_qty;

        // 记录成交
        {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_TRADE;
            rsp.data.trade.price         = best_bid->price;
            rsp.data.trade.quantity      = trade_qty;
            rsp.data.trade.buyer_id      = best_bid->user_id;
            rsp.data.trade.seller_id     = order.user_id;
            rsp.data.trade.buy_order_id  = best_bid->order_id;
            rsp.data.trade.sell_order_id = order.order_id;
        }

        if (best_bid->remaining_qty == 0)
        {
            best_bid->status = OrderStatus::FILLED;
            order_book_.removeOrder(best_bid->order_id, best_bid->user_id);
        }
        else
        {
            best_bid->status = OrderStatus::PARTIALLY_FILLED;
        }

        order.status = (order.remaining_qty == 0)
            ? OrderStatus::FILLED
            : OrderStatus::PARTIALLY_FILLED;
    }
}
