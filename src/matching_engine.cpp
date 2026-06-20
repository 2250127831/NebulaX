#include "matching_engine.h"
#include "logger.h"
#include "wal.h"
#include "trade_pool.h"
#include <algorithm>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

void MatchingEngine::processNewOrder(
    Side side,
    uint32_t price,
    uint32_t quantity,
    uint64_t user_id,
    std::vector<BinaryResponse>& out)
{
    if (metrics_) metrics_->new_orders++;

    if (side == Side::INVALID)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_SIDE);
        if (metrics_) metrics_->errors++;
        return;
    }

    if (price == 0 || quantity == 0 || user_id == 0)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_PRICE_QTY_USER);
        if (metrics_) metrics_->errors++;
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

    // ── WAL ──
    if (wal_) {
        WalEntry e;
        e.type = 0x01; e.side = (side == Side::BUY) ? 0x01 : 0x02;
        e.price = price; e.quantity = quantity;
        e.user_id = user_id; e.order_id = order.order_id;
        e.wal_seq = next_sequence_;
        wal_->append(e);
    }

    if (side == Side::BUY)
        matchBuyOrder(order, out);
    else
        matchSellOrder(order, out);

    if (order.status == OrderStatus::OPEN ||
        order.status == OrderStatus::PARTIALLY_FILLED)
    {
        if (!order_book_.addOrder(order)) {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_ERROR;
            rsp.data.error.code = static_cast<uint16_t>(ErrorCode::POOL_FULL);
            if (metrics_) { metrics_->errors++; metrics_->order_pool_used = order_book_.poolUsage(); }
        } else {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_OK;
            rsp.data.ack.order_id = order.order_id;
            if (metrics_) metrics_->order_pool_used = order_book_.poolUsage();
        }
    }
    else if (order.status == OrderStatus::FILLED)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_FILLED;
        rsp.data.ack.order_id = order.order_id;
        if (metrics_) metrics_->order_pool_used = order_book_.poolUsage();
    }

}

void MatchingEngine::processCancel(
    uint64_t order_id,
    uint64_t user_id,
    std::vector<BinaryResponse>& out)
{
    if (metrics_) metrics_->cancels++;

    // ── WAL ──
    if (wal_) {
        WalEntry e;
        e.type = 0x02; e.side = 0;
        e.price = 0; e.quantity = 0;
        e.user_id = user_id; e.order_id = order_id;
        e.wal_seq = next_sequence_;
        wal_->append(e);
    }

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
        if (metrics_) metrics_->errors++;
    }
}

void MatchingEngine::saveSnapshot(const char* path) const
{
    order_book_.saveSnapshot(path);
}

void MatchingEngine::recoverFromWal(const char* wal_path)
{
    WalReader reader;
    if (!reader.open(wal_path)) {
        LOG_WARN("no WAL to replay");
        return;
    }

    size_t n = reader.entryCount();
    if (n > WAL_ENTRIES) n = WAL_ENTRIES;
    for (size_t i = 0; i < n; i++) {
        auto* entry = reader.entryAt(i);
        if (!entry) break;

        // 幂等回放
        if (entry->type == 0x01) {  // NEW
            if (order_book_.findOrder(entry->order_id)) continue;

            Order order;
            order.user_id = entry->user_id;
            order.order_id = entry->order_id;
            order.side = (entry->side == 0x01) ? Side::BUY : Side::SELL;
            order.price = entry->price;
            order.original_qty = entry->quantity;
            order.remaining_qty = entry->quantity;
            order.sequence = entry->wal_seq;
            order.status = OrderStatus::OPEN;

            // 按 NEW → CANCEL 的顺序回放，CANCEL 在下一步处理
            // 先尝试撮合
            std::vector<BinaryResponse> tmp;
            if (order.side == Side::BUY) matchBuyOrder(order, tmp);
            else matchSellOrder(order, tmp);

            if (order.status == OrderStatus::OPEN || order.status == OrderStatus::PARTIALLY_FILLED)
                order_book_.addOrder(order);
            // FILLED 订单不加入池
        } else if (entry->type == 0x02) {  // CANCEL
            order_book_.removeOrder(entry->order_id, entry->user_id);
        }

        if (entry->wal_seq >= next_sequence_) next_sequence_ = entry->wal_seq + 1;
        if (entry->order_id >= next_order_id_) next_order_id_ = entry->order_id + 1;
    }

    LOG_INFO("WAL replay: %zu entries, orders=%zu", n, order_book_.poolUsage());
    if (metrics_) metrics_->order_pool_used = order_book_.poolUsage();
    reader.close();
}

void MatchingEngine::recoverFromShared(Order* storage, size_t capacity)
{
    // 重建空闲链表：order_id==0 的槽位视为空闲
    order_book_.getPool().rebuildFreelist();

    // 遍历池，取出活跃订单逐个 addOrder（重建 bids_/asks_ + order_index_）
    // 实际 pool 中已有数据，addOrder 会 allocate 相同 slot 并覆盖
    uint64_t count = 0;
    for (size_t i = 0; i < capacity; i++) {
        Order& o = storage[i];
        if (o.order_id == 0) continue;          // 空闲槽
        if (o.status == OrderStatus::FILLED ||
            o.status == OrderStatus::CANCELLED) continue;

        Order copy = o;
        copy.prev_idx = UINT32_MAX;
        copy.next_idx = UINT32_MAX;
        if (order_book_.addOrder(copy))
            count++;

        if (o.order_id >= next_order_id_) next_order_id_ = o.order_id + 1;
        if (o.sequence >= next_sequence_) next_sequence_ = o.sequence + 1;
    }
    LOG_INFO("recovered %lu orders from shared memory", count);
    if (metrics_) metrics_->order_pool_used = order_book_.poolUsage();
}

void MatchingEngine::loadSnapshot(const char* path)
{
    uint64_t max_seq = 0, max_id = 0;
    order_book_.loadSnapshot(path, max_seq, max_id);
    if (max_seq > 0) next_sequence_ = max_seq + 1;
    if (max_id  > 0) next_order_id_ = max_id + 1;
    LOG_INFO("snapshot loaded: orders=%lu seq=%lu id=%lu",
             order_book_.poolUsage(), next_sequence_, next_order_id_);
    if (metrics_) metrics_->order_pool_used = order_book_.poolUsage();
}

void MatchingEngine::checkpointIfNeeded() {
    if (!wal_ || !book_base_ || book_size_ == 0) return;
    if (!wal_->needCheckpoint()) return;

    LOG_INFO("WAL near wrap, checkpoint (total=%lu)", wal_->totalWritten());

    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/tmp/nebulaX_checkpoint.dat", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, book_base_, book_size_);
            uint64_t pos = wal_->curPosition();
            write(fd, &pos, sizeof(pos));
            close(fd);
        }
        _exit(0);
    }
    // 父进程不 waitpid，子进程变僵尸，下次 tick 收割
}

void MatchingEngine::getBook(BinaryResponse& out) const
{
    if (metrics_) metrics_->book_queries++;
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
        Order* best_ask = order_book_.getBestAsk(order.user_id);
        if (!best_ask) break;

        if (order.price < best_ask->price) break;

        uint32_t trade_qty = std::min(order.remaining_qty, best_ask->remaining_qty);

        order.remaining_qty -= trade_qty;
        order.filled_qty   += trade_qty;

        order_book_.reduceOrderQty(best_ask, trade_qty);

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
            if (metrics_) metrics_->trades++;

            // 写 TradePool
            if (trade_pool_) {
                static uint64_t trade_id = 0;
                auto idx = trade_pool_->write_idx.fetch_add(1, std::memory_order_relaxed) % TRADE_CAPACITY;
                auto& t = trade_pool_->entries[idx];
                t.trade_id = ++trade_id;
                t.buy_order_id = order.order_id;
                t.sell_order_id = best_ask->order_id;
                t.price = best_ask->price;
                t.quantity = trade_qty;
                t.buyer_id = order.user_id;
                t.seller_id = best_ask->user_id;
                t.seq = next_sequence_;
            }
        }

        if (best_ask->remaining_qty == 0)
        {
            best_ask->status = OrderStatus::FILLED;
            // removeOrder 后 best_ask 悬空，循环顶部重新获取
            order_book_.removeOrder(best_ask);
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
        Order* best_bid = order_book_.getBestBid(order.user_id);
        if (!best_bid) break;

        if (order.price > best_bid->price) break;

        uint32_t trade_qty = std::min(order.remaining_qty, best_bid->remaining_qty);

        order.remaining_qty -= trade_qty;
        order.filled_qty   += trade_qty;

        order_book_.reduceOrderQty(best_bid, trade_qty);

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
            if (metrics_) metrics_->trades++;

            // 写 TradePool
            if (trade_pool_) {
                static uint64_t trade_id = 0;
                auto idx = trade_pool_->write_idx.fetch_add(1, std::memory_order_relaxed) % TRADE_CAPACITY;
                auto& t = trade_pool_->entries[idx];
                t.trade_id = ++trade_id;
                t.buy_order_id = best_bid->order_id;
                t.sell_order_id = order.order_id;
                t.price = best_bid->price;
                t.quantity = trade_qty;
                t.buyer_id = best_bid->user_id;
                t.seller_id = order.user_id;
                t.seq = next_sequence_;
            }
        }

        if (best_bid->remaining_qty == 0)
        {
            best_bid->status = OrderStatus::FILLED;
            order_book_.removeOrder(best_bid);
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
