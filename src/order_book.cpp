#include "order_book.h"
#include <inttypes.h>

bool OrderBook::addOrder(const Order& order)
{
    if (order.side == Side::INVALID)
        return false;

    Order* new_order = pool_.allocate();
    if (!new_order) return false;

    *new_order = order;
    uint32_t idx = pool_.indexOf(new_order);

    PriceLevel& level = (order.side == Side::BUY)
        ? bids_[order.price]
        : asks_[order.price];

    if (level.count == 0) {
        level.head_idx = level.tail_idx = idx;
        new_order->prev_idx = UINT32_MAX;
        new_order->next_idx = UINT32_MAX;
    } else {
        new_order->prev_idx = level.tail_idx;
        new_order->next_idx = UINT32_MAX;
        pool_.at(level.tail_idx)->next_idx = idx;
        level.tail_idx = idx;
    }
    level.count++;
    level.total_qty += order.remaining_qty;

    order_index_.insert(order.order_id, new_order);
    return true;
}

void OrderBook::reduceOrderQty(Order* order, uint32_t amount)
{
    order->remaining_qty -= amount;
    order->filled_qty += amount;

    PriceLevel& level = (order->side == Side::BUY)
        ? bids_[order->price] : asks_[order->price];
    level.total_qty -= amount;
}

void OrderBook::removeOrder(Order* order)
{
    uint32_t idx = pool_.indexOf(order);

    auto findLevel = [&](auto& map) -> PriceLevel* {
        auto it = map.find(order->price);
        return (it != map.end()) ? &it->second : nullptr;
    };
    PriceLevel* level = (order->side == Side::BUY)
        ? findLevel(bids_) : findLevel(asks_);
    if (!level) return;

    PriceLevel& lvl = *level;

    if (order->prev_idx != UINT32_MAX)
        pool_.at(order->prev_idx)->next_idx = order->next_idx;
    if (order->next_idx != UINT32_MAX)
        pool_.at(order->next_idx)->prev_idx = order->prev_idx;
    if (lvl.head_idx == idx)
        lvl.head_idx = order->next_idx;
    if (lvl.tail_idx == idx)
        lvl.tail_idx = order->prev_idx;

    lvl.count--;
    lvl.total_qty -= order->remaining_qty;
    if (lvl.count == 0) {
        if (order->side == Side::BUY)
            bids_.erase(order->price);
        else
            asks_.erase(order->price);
    }

    order_index_.erase(order->order_id);
    pool_.deallocate(idx);
}

bool OrderBook::removeOrder(uint64_t order_id, uint64_t user_id)
{
    Order* order = order_index_.find(order_id);
    if (!order) return false;
    if (order->user_id != user_id) return false;

    removeOrder(order);
    return true;
}

Order* OrderBook::getBestBid(uint64_t exclude_user_id)
{
    if (bids_.empty()) return nullptr;

    if (!exclude_user_id) {
        PriceLevel& level = bids_.begin()->second;
        return (level.count > 0) ? pool_.at(level.head_idx) : nullptr;
    }

    for (auto& [price, level] : bids_) {
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            Order* o = pool_.at(idx);
            if (o->user_id != exclude_user_id) return o;
            idx = o->next_idx;
        }
    }
    return nullptr;
}

Order* OrderBook::getBestAsk(uint64_t exclude_user_id)
{
    if (asks_.empty()) return nullptr;

    if (!exclude_user_id) {
        PriceLevel& level = asks_.begin()->second;
        return (level.count > 0) ? pool_.at(level.head_idx) : nullptr;
    }

    for (auto& [price, level] : asks_) {
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            Order* o = pool_.at(idx);
            if (o->user_id != exclude_user_id) return o;
            idx = o->next_idx;
        }
    }
    return nullptr;
}

TopOfBook OrderBook::getTopOfBook() const
{
    TopOfBook tob;

    if (!bids_.empty()) {
        const PriceLevel& level = bids_.begin()->second;
        tob.bid_price = bids_.begin()->first;
        tob.bid_volume = level.total_qty;
    }

    if (!asks_.empty()) {
        const PriceLevel& level = asks_.begin()->second;
        tob.ask_price = asks_.begin()->first;
        tob.ask_volume = level.total_qty;
    }

    return tob;
}

Order* OrderBook::findOrder(uint64_t order_id)
{
    return order_index_.find(order_id);
}

void OrderBook::printBook(int levels) const
{
    printf("\n==============BOOK_BEGIN==============\n");

    int ask_count = 0;
    printf("\nASKS:\n");
    for (const auto& [price, level] : asks_) {
        if (ask_count >= levels) break;
        uint32_t total_vol = 0;
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            total_vol += pool_.at(idx)->remaining_qty;
            idx = pool_.at(idx)->next_idx;
        }
        printf("%d : price = %" PRIu32 "  ->  total = %" PRIu32 "\n",
               ask_count, price, total_vol);
        ++ask_count;
    }

    int bid_count = 0;
    printf("\nBIDS:\n");
    for (const auto& [price, level] : bids_) {
        if (bid_count >= levels) break;
        uint32_t total_vol = 0;
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            total_vol += pool_.at(idx)->remaining_qty;
            idx = pool_.at(idx)->next_idx;
        }
        printf("%d : price = %" PRIu32 "  ->  total = %" PRIu32 "\n",
               bid_count, price, total_vol);
        ++bid_count;
    }
    printf("\n==============BOOK_END==============\n");
}

std::string OrderBook::getBookString(int levels) const
{
    std::ostringstream oss;
    oss << "\n==============BOOK_BEGIN==============\n";

    int ask_count = 0;
    oss << "\nASKS:\n";
    for (const auto& [price, level] : asks_) {
        if (ask_count >= levels) break;
        uint32_t total_vol = 0;
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            total_vol += pool_.at(idx)->remaining_qty;
            idx = pool_.at(idx)->next_idx;
        }
        oss << ask_count << " : price = " << price
            << "  ->  total = " << total_vol << "\n";
        ++ask_count;
    }

    int bid_count = 0;
    oss << "\nBIDS:\n";
    for (const auto& [price, level] : bids_) {
        if (bid_count >= levels) break;
        uint32_t total_vol = 0;
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            total_vol += pool_.at(idx)->remaining_qty;
            idx = pool_.at(idx)->next_idx;
        }
        oss << bid_count << " : price = " << price
            << "  ->  total = " << total_vol << "\n";
        ++bid_count;
    }

    oss << "==============BOOK_END==============\n";
    return oss.str();
}
