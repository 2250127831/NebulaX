#include "order_book.h"
#include "logger.h"
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

bool OrderBook::addOrder(const Order& order)
{
    if (order.side == Side::INVALID)
        return false;

    Order* new_order = pool_->allocate();
    if (!new_order) return false;

    *new_order = order;
    uint32_t idx = pool_->indexOf(new_order);

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
        pool_->at(level.tail_idx)->next_idx = idx;
        level.tail_idx = idx;
    }
    level.count++;
    level.total_qty += order.remaining_qty;

    index_->insert(order.order_id, new_order);
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

void OrderBook::reduceQtyCancel(Order* order, uint32_t amount)
{
    order->remaining_qty -= amount;   // 只减剩余，不加 filled_qty

    PriceLevel& level = (order->side == Side::BUY)
        ? bids_[order->price] : asks_[order->price];
    level.total_qty -= amount;
}

void OrderBook::removeOrder(Order* order)
{
    uint32_t idx = pool_->indexOf(order);

    auto findLevel = [&](auto& map) -> PriceLevel* {
        auto it = map.find(order->price);
        return (it != map.end()) ? &it->second : nullptr;
    };
    PriceLevel* level = (order->side == Side::BUY)
        ? findLevel(bids_) : findLevel(asks_);
    if (!level) return;

    PriceLevel& lvl = *level;

    if (order->prev_idx != UINT32_MAX)
        pool_->at(order->prev_idx)->next_idx = order->next_idx;
    if (order->next_idx != UINT32_MAX)
        pool_->at(order->next_idx)->prev_idx = order->prev_idx;
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

    index_->erase(order->order_id);
    pool_->deallocate(idx);
}

bool OrderBook::removeOrder(uint64_t order_id, uint64_t user_id)
{
    Order* order = index_->find(order_id);
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
        return (level.count > 0) ? pool_->at(level.head_idx) : nullptr;
    }

    for (auto& [price, level] : bids_) {
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            Order* o = pool_->at(idx);
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
        return (level.count > 0) ? pool_->at(level.head_idx) : nullptr;
    }

    for (auto& [price, level] : asks_) {
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            Order* o = pool_->at(idx);
            if (o->user_id != exclude_user_id) return o;
            idx = o->next_idx;
        }
    }
    return nullptr;
}

uint32_t OrderBook::availableQty(Side side, uint32_t price) const
{
    uint32_t total = 0;
    if (side == Side::BUY) {
        // 买单吃卖盘：asks_ 升序，价格 ≤ 我方买价才可吃
        for (auto& [p, level] : asks_) {
            if (p > price) break;
            uint32_t idx = level.head_idx;
            while (idx != UINT32_MAX) {
                total += pool_->at(idx)->remaining_qty;
                idx = pool_->at(idx)->next_idx;
            }
        }
    } else {
        // 卖单吃买盘：bids_ 降序，价格 ≥ 我方卖价才可吃
        for (auto& [p, level] : bids_) {
            if (p < price) break;
            uint32_t idx = level.head_idx;
            while (idx != UINT32_MAX) {
                total += pool_->at(idx)->remaining_qty;
                idx = pool_->at(idx)->next_idx;
            }
        }
    }
    return total;
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
    return index_->find(order_id);
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
            total_vol += pool_->at(idx)->remaining_qty;
            idx = pool_->at(idx)->next_idx;
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
            total_vol += pool_->at(idx)->remaining_qty;
            idx = pool_->at(idx)->next_idx;
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
            total_vol += pool_->at(idx)->remaining_qty;
            idx = pool_->at(idx)->next_idx;
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
            total_vol += pool_->at(idx)->remaining_qty;
            idx = pool_->at(idx)->next_idx;
        }
        oss << bid_count << " : price = " << price
            << "  ->  total = " << total_vol << "\n";
        ++bid_count;
    }

    oss << "==============BOOK_END==============\n";
    return oss.str();
}

uint64_t OrderBook::saveSnapshot(const char* path) const
{
    uint64_t max_seq = 0, max_id = 0;
    int fd = open(path, O_CREAT|O_WRONLY|O_TRUNC, 0644);
    if (fd < 0) return 0;

    uint32_t magic = 0x4E4253;
    write(fd, &magic, sizeof(magic));

    for (auto& [price, level] : bids_) {
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            const Order* o = pool_->at(idx);
            if (o->sequence > max_seq) max_seq = o->sequence;
            if (o->order_id  > max_id) max_id  = o->order_id;
            write(fd, o, sizeof(Order));
            idx = o->next_idx;
        }
    }
    for (auto& [price, level] : asks_) {
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            const Order* o = pool_->at(idx);
            if (o->sequence > max_seq) max_seq = o->sequence;
            if (o->order_id  > max_id) max_id  = o->order_id;
            write(fd, o, sizeof(Order));
            idx = o->next_idx;
        }
    }
    close(fd);
    return max_id;
}

void OrderBook::loadSnapshot(const char* path, uint64_t& max_seq_out, uint64_t& max_id_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    uint32_t magic = 0;
    read(fd, &magic, sizeof(magic));
    if (magic != 0x4E4253) { close(fd); return; }

    uint32_t loaded = 0;
    while (true) {
        Order o;
        if (read(fd, &o, sizeof(Order)) != sizeof(Order)) break;
        o.prev_idx = UINT32_MAX;
        o.next_idx = UINT32_MAX;
        o.pool_next_free = UINT32_MAX;
        if (o.sequence > max_seq_out) max_seq_out = o.sequence;
        if (o.order_id  > max_id_out) max_id_out  = o.order_id;
        if (!addOrder(o))
            LOG_ERROR("loadSnapshot: addOrder failed");
        loaded++;
    }
    LOG_INFO("loadSnapshot: %u orders", loaded);
    close(fd);
}
