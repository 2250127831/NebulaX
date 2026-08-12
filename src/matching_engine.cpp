#include "matching_engine.h"
#include "logger.h"
#include "wal.h"
#include "trade_pool.h"
#include <algorithm>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

// ── 构造 ──

MatchingEngine::MatchingEngine(OrderPool& shared_pool, OrderMap& shared_index, IOCounters* metrics)
    : books_(new std::atomic<OrderBook*>[kMaxLocate])
    , shared_pool_(&shared_pool)
    , shared_index_(&shared_index)
    , metrics_(metrics)
{
    for (uint32_t i = 0; i < kMaxLocate; ++i)
        books_[i].store(nullptr, std::memory_order_relaxed);
    if (metrics_) metrics_->order_pool_capacity = shared_pool_->capacity();
}

MatchingEngine::MatchingEngine(size_t pool_capacity, IOCounters* metrics)
    : books_(new std::atomic<OrderBook*>[kMaxLocate])
    , shared_pool_(nullptr)
    , shared_index_(nullptr)
    , owned_pool_(new OrderPool(pool_capacity))
    , owned_index_(new OrderMap(pool_capacity))
    , metrics_(metrics)
{
    shared_pool_ = owned_pool_.get();
    shared_index_ = owned_index_.get();
    for (uint32_t i = 0; i < kMaxLocate; ++i)
        books_[i].store(nullptr, std::memory_order_relaxed);
    if (metrics_) metrics_->order_pool_capacity = shared_pool_->capacity();
}

MatchingEngine::MatchingEngine(OrderPool* external_pool, IOCounters* metrics)
    : books_(new std::atomic<OrderBook*>[kMaxLocate])
    , shared_pool_(external_pool)
    , shared_index_(nullptr)
    , owned_index_(new OrderMap(external_pool->capacity()))
    , metrics_(metrics)
{
    shared_index_ = owned_index_.get();
    for (uint32_t i = 0; i < kMaxLocate; ++i)
        books_[i].store(nullptr, std::memory_order_relaxed);
    if (metrics_) metrics_->order_pool_capacity = shared_pool_->capacity();
}

OrderBook* MatchingEngine::book_for(uint64_t locate)
{
    if (locate >= kMaxLocate) return nullptr;
    OrderBook* b = books_[locate].load(std::memory_order_acquire);
    if (b) return b;
    // 并发首次建簿：CAS 抢占。失败则用先到者的簿（回测多线程驱动不同 symbol 安全）。
    auto* nb = new OrderBook(*shared_pool_, *shared_index_);
    OrderBook* expected = nullptr;
    if (books_[locate].compare_exchange_strong(expected, nb,
                std::memory_order_release, std::memory_order_acquire)) {
        return nb;
    }
    delete nb;
    return books_[locate].load(std::memory_order_acquire);
}

// ── ITCH 语义 API ──

void MatchingEngine::processAdd(
    uint64_t locate, uint64_t order_ref, Side side,
    uint32_t price, uint32_t quantity, uint64_t user_id,
    std::vector<BinaryResponse>& out, OrderTif tif)
{
    if (metrics_) metrics_->new_orders++;

    OrderBook* book = book_for(locate);
    if (!book) {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_COMMAND_TYPE);
        if (metrics_) metrics_->errors++;
        return;
    }

    if (side == Side::INVALID)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_SIDE);
        if (metrics_) metrics_->errors++;
        return;
    }

    bool is_market = (tif == OrderTif::IOC || tif == OrderTif::FOK) && price == 0;
    // 市价单 price 可忽略（market 价 = 对手盘价）；限价单必须有效价格
    if ((!is_market && price == 0) || quantity == 0 || user_id == 0 || order_ref == 0)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_PRICE_QTY_USER);
        if (metrics_) metrics_->errors++;
        return;
    }

    // FOK：先检查能否全部成交（对手盘可吃量 ≥ 需求量），不能则整个拒绝。
    // 市价 FOK（price=0）用盘口价；限价 FOK（price>0）用限价扫描。
    if (tif == OrderTif::FOK) {
        if (!canFullFill(*book, side, is_market ? (side == Side::BUY ? UINT32_MAX : 0) : price,
                         quantity)) {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_ERROR;
            rsp.data.error.code = static_cast<uint16_t>(ErrorCode::FOK_NO_FULL_FILL);
            if (metrics_) metrics_->errors++;
            return;
        }
    }

    Order order;
    order.user_id       = user_id;
    order.order_id      = order_ref;   // order_ref 外部提供（ITCH 全局唯一）
    order.side          = side;
    // 市价单：极端价格保证撮合触发（成交价仍用对手盘价，非订单价）。IOC 不挂簿。
    order.price         = is_market ? (side == Side::BUY ? UINT32_MAX : 0) : price;
    order.original_qty  = quantity;
    order.remaining_qty = quantity;
    order.filled_qty    = 0;
    order.sequence      = next_sequence_.fetch_add(1);
    order.status        = OrderStatus::OPEN;

    // ── WAL（市价单按极端价记录，恢复时语义一致）──
    if (wal_) {
        WalEntry e;
        e.type = 0x01; e.side = (side == Side::BUY) ? 0x01 : 0x02;
        e.locate = (uint16_t)locate;
        e.price = order.price; e.quantity = quantity;
        e.user_id = user_id; e.order_id = order_ref;
        e.wal_seq = next_sequence_.load();
        wal_->append(e);
    }

    if (side == Side::BUY)
        matchBuyOrder(locate, *book, order, out);
    else
        matchSellOrder(locate, *book, order, out);

    // ── 成交后状态 ──
    if (order.status == OrderStatus::FILLED)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_FILLED;
        rsp.data.ack.order_id = order_ref;
        if (metrics_) metrics_->order_pool_used = book->poolUsage();
    }
    else if (tif == OrderTif::IOC)
    {
        // 市价单 IOC：未全成（部分或零成交）→ 剩余作废不挂簿。
        // 零成交（无对手盘）→ RSP_ERROR 表示无流动性（TcpTradeServer 转 'F' filled=0）。
        if (order.filled_qty == 0) {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_ERROR;
            rsp.data.error.code = static_cast<uint16_t>(ErrorCode::MKT_NO_LIQUIDITY);
            if (metrics_) metrics_->errors++;
        }
        // 部分成交的市价单：剩余作废，已成交部分已通过 RSP_TRADE 回报
    }
    else if (tif == OrderTif::FOK)
    {
        // FOK：已确认全成交才到这，理论上 FILLED。防御：若未全成（竞态），作废不挂簿。
        // 市价 FOK 剩余作废；限价 FOK 未全成则整个作废（FOK 语义：不留挂单）。
        if (order.filled_qty < quantity) {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_ERROR;
            rsp.data.error.code = static_cast<uint16_t>(ErrorCode::FOK_NO_FULL_FILL);
            if (metrics_) metrics_->errors++;
        }
    }
    else if (order.status == OrderStatus::OPEN ||
             order.status == OrderStatus::PARTIALLY_FILLED)
    {
        // DAY 限价单：剩余挂簿
        if (!book->addOrder(order)) {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_ERROR;
            rsp.data.error.code = static_cast<uint16_t>(ErrorCode::POOL_FULL);
            if (metrics_) { metrics_->errors++; metrics_->order_pool_used = book->poolUsage(); }
        } else {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_OK;
            rsp.data.ack.order_id = order_ref;
            if (metrics_) metrics_->order_pool_used = book->poolUsage();
        }
    }
}

void MatchingEngine::processCancel(
    uint64_t locate, uint64_t order_ref, uint64_t user_id,
    std::vector<BinaryResponse>& out)
{
    if (metrics_) metrics_->cancels++;

    OrderBook* book = book_for(locate);
    if (!book) {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::ORDER_NOT_FOUND);
        if (metrics_) metrics_->errors++;
        return;
    }

    // ── WAL ──
    if (wal_) {
        WalEntry e;
        e.type = 0x02; e.side = 0;
        e.locate = (uint16_t)locate;
        e.price = 0; e.quantity = 0;
        e.user_id = user_id; e.order_id = order_ref;
        e.wal_seq = next_sequence_.load();
        wal_->append(e);
    }

    bool removed = book->removeOrder(order_ref, user_id);

    auto& rsp = out.emplace_back();
    if (removed)
    {
        rsp.type = RSP_CANCELLED;
        rsp.data.ack.order_id = order_ref;
    }
    else
    {
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::ORDER_NOT_FOUND);
        if (metrics_) metrics_->errors++;
    }
}

void MatchingEngine::processCancelShares(
    uint64_t locate, uint64_t order_ref, uint32_t shares,
    uint64_t user_id, std::vector<BinaryResponse>& out)
{
    if (metrics_) metrics_->cancels++;

    OrderBook* book = book_for(locate);
    if (!book) {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::ORDER_NOT_FOUND);
        if (metrics_) metrics_->errors++;
        return;
    }

    // ── WAL ──
    if (wal_) {
        WalEntry e;
        e.type = 0x03; e.side = 0;   // 0x03 = 部分撤单
        e.locate = (uint16_t)locate;
        e.price = 0; e.quantity = shares;
        e.user_id = user_id; e.order_id = order_ref;
        e.wal_seq = next_sequence_.load();
        wal_->append(e);
    }

    Order* o = book->findOrder(order_ref);
    if (!o || o->user_id != user_id) {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::ORDER_NOT_FOUND);
        if (metrics_) metrics_->errors++;
        return;
    }
    if (shares >= o->remaining_qty) {
        // 撤单量 ≥ 剩余 → 整笔撤
        book->removeOrder(o);
        auto& rsp = out.emplace_back();
        rsp.type = RSP_CANCELLED;
        rsp.data.ack.order_id = order_ref;
    } else {
        // 部分撤：减少剩余量 + 档量
        o->remaining_qty -= shares;
        book->reduceOrderQty(o, shares);
        auto& rsp = out.emplace_back();
        rsp.type = RSP_OK;
        rsp.data.ack.order_id = order_ref;
    }
}

void MatchingEngine::processReplace(
    uint64_t locate, uint64_t old_ref, uint64_t new_ref, Side side,
    uint32_t price, uint32_t quantity, uint64_t user_id,
    std::vector<BinaryResponse>& out)
{
    if (metrics_) metrics_->cancels++;

    OrderBook* book = book_for(locate);
    if (!book) {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::ORDER_NOT_FOUND);
        if (metrics_) metrics_->errors++;
        return;
    }

    // ── WAL ──
    if (wal_) {
        WalEntry e;
        e.type = 0x04; e.side = 0;   // 0x04 = 改单
        e.locate = (uint16_t)locate;
        e.price = price; e.quantity = quantity;
        e.user_id = user_id; e.order_id = new_ref;
        e.wal_seq = next_sequence_.load();
        wal_->append(e);
    }

    Order* old = book->findOrder(old_ref);
    if (!old || old->user_id != user_id) {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_ERROR;
        rsp.data.error.code = static_cast<uint16_t>(ErrorCode::ORDER_NOT_FOUND);
        if (metrics_) metrics_->errors++;
        return;
    }

    // 作废旧单，新单以新价/新量挂出（U 无方向，side 由调用方传入旧单方向）
    // 新单身份用 new_ref（后续 D/X/U 引用新 ref 时 user_id 匹配）
    book->removeOrder(old);

    Order order;
    order.user_id       = new_ref;
    order.order_id      = new_ref;
    order.side          = side;
    order.price         = price;
    order.original_qty  = quantity;
    order.remaining_qty = quantity;
    order.filled_qty    = 0;
    order.sequence      = next_sequence_.fetch_add(1);
    order.status        = OrderStatus::OPEN;

    if (side == Side::BUY)
        matchBuyOrder(locate, *book, order, out);
    else
        matchSellOrder(locate, *book, order, out);

    if (order.status == OrderStatus::OPEN ||
        order.status == OrderStatus::PARTIALLY_FILLED)
    {
        if (!book->addOrder(order)) {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_ERROR;
            rsp.data.error.code = static_cast<uint16_t>(ErrorCode::POOL_FULL);
            if (metrics_) { metrics_->errors++; metrics_->order_pool_used = book->poolUsage(); }
        } else {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_OK;
            rsp.data.ack.order_id = new_ref;
            if (metrics_) metrics_->order_pool_used = book->poolUsage();
        }
    }
    else if (order.status == OrderStatus::FILLED)
    {
        auto& rsp = out.emplace_back();
        rsp.type = RSP_FILLED;
        rsp.data.ack.order_id = new_ref;
    }
}

// ── 单簿兼容 API（locate=0，P5 前 TcpServer 沿用）──

void MatchingEngine::processNewOrder(
    Side side, uint32_t price, uint32_t quantity, uint64_t user_id,
    std::vector<BinaryResponse>& out)
{
    processAdd(0, next_order_id_.fetch_add(1), side, price, quantity, user_id, out);
}

void MatchingEngine::processCancel(
    uint64_t order_id, uint64_t user_id,
    std::vector<BinaryResponse>& out)
{
    processCancel(0, order_id, user_id, out);
}

// ── 快照 / 恢复 ──

void MatchingEngine::saveSnapshot(const char* path) const
{
    // 单簿兼容：locate=0 的簿（P4 分簿快照格式留待 WAL locate 字段落地后统一）
    auto* book = books_[0].load(std::memory_order_acquire);
    if (book) book->saveSnapshot(path);
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

        // 按 locate 路由到对应簿（分簿恢复）
        uint64_t locate = entry->locate;
        OrderBook* book = book_for(locate);
        if (!book) continue;

        // 幂等回放
        if (entry->type == 0x01) {  // NEW
            if (book->findOrder(entry->order_id)) continue;

            Order order;
            order.user_id = entry->user_id;
            order.order_id = entry->order_id;
            order.side = (entry->side == 0x01) ? Side::BUY : Side::SELL;
            order.price = entry->price;
            order.original_qty = entry->quantity;
            order.remaining_qty = entry->quantity;
            order.sequence = entry->wal_seq;
            order.status = OrderStatus::OPEN;

            std::vector<BinaryResponse> tmp;
            if (order.side == Side::BUY) matchBuyOrder(locate, *book, order, tmp);
            else matchSellOrder(locate, *book, order, tmp);

            if (order.status == OrderStatus::OPEN || order.status == OrderStatus::PARTIALLY_FILLED)
                book->addOrder(order);
            // FILLED 订单不加入池
        } else if (entry->type == 0x02) {  // CANCEL
            book->removeOrder(entry->order_id, entry->user_id);
        } else if (entry->type == 0x03) {  // 部分撤单
            Order* o = book->findOrder(entry->order_id);
            if (o && entry->quantity > 0) {
                if (entry->quantity >= o->remaining_qty) book->removeOrder(o);
                else { o->remaining_qty -= entry->quantity; book->reduceOrderQty(o, entry->quantity); }
            }
        } else if (entry->type == 0x04) {  // 改单
            Order* o = book->findOrder(entry->order_id);
            if (o && o->user_id == entry->user_id) book->removeOrder(o);
        }

        if (entry->wal_seq >= next_sequence_.load()) next_sequence_.store(entry->wal_seq + 1);
        if (entry->order_id >= next_order_id_.load()) next_order_id_.store(entry->order_id + 1);
    }

    LOG_INFO("WAL replay: %zu entries", n);
    if (metrics_) metrics_->order_pool_used = shared_pool_->size();
    reader.close();
}

void MatchingEngine::recoverFromShared(Order* storage, size_t capacity)
{
    // 重建空闲链表：order_id==0 的槽位视为空闲
    shared_pool_->rebuildFreelist();
    OrderBook* book = book_for(0);

    uint64_t count = 0;
    for (size_t i = 0; i < capacity; i++) {
        Order& o = storage[i];
        if (o.order_id == 0) continue;          // 空闲槽
        if (o.status == OrderStatus::FILLED ||
            o.status == OrderStatus::CANCELLED) continue;

        Order copy = o;
        copy.prev_idx = UINT32_MAX;
        copy.next_idx = UINT32_MAX;
        if (book->addOrder(copy))
            count++;

        if (o.order_id >= next_order_id_.load()) next_order_id_.store(o.order_id + 1);
        if (o.sequence >= next_sequence_.load()) next_sequence_.store(o.sequence + 1);
    }
    LOG_INFO("recovered %lu orders from shared memory", count);
    if (metrics_) metrics_->order_pool_used = book->poolUsage();
}

void MatchingEngine::loadSnapshot(const char* path)
{
    OrderBook* book = book_for(0);
    uint64_t max_seq = 0, max_id = 0;
    book->loadSnapshot(path, max_seq, max_id);
    if (max_seq > 0) next_sequence_.store(max_seq + 1);
    if (max_id  > 0) next_order_id_.store(max_id + 1);
    LOG_INFO("snapshot loaded: orders=%lu seq=%lu id=%lu",
             book->poolUsage(), next_sequence_.load(), next_order_id_.load());
    if (metrics_) metrics_->order_pool_used = book->poolUsage();
}

// ── checkpoint / getBook ──

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

void MatchingEngine::getBook(uint64_t locate, BinaryResponse& out) const
{
    if (metrics_) metrics_->book_queries++;
    out.type = RSP_BOOK;
    out.data.book.locate = locate;

    if (locate >= kMaxLocate) {
        out.data.book.bid_price = 0; out.data.book.bid_volume = 0;
        out.data.book.ask_price = 0; out.data.book.ask_volume = 0;
        return;
    }
    auto* book = books_[locate].load(std::memory_order_acquire);
    if (!book) {
        out.data.book.bid_price = 0; out.data.book.bid_volume = 0;
        out.data.book.ask_price = 0; out.data.book.ask_volume = 0;
        return;
    }
    TopOfBook tob = book->getTopOfBook();
    out.data.book.bid_price  = tob.bid_price;
    out.data.book.bid_volume = tob.bid_volume;
    out.data.book.ask_price  = tob.ask_price;
    out.data.book.ask_volume = tob.ask_volume;
}

// ── FOK 预检查 ──

bool MatchingEngine::canFullFill(const OrderBook& book, Side side,
                                 uint32_t price, uint32_t quantity) const {
    // 对手盘可吃总量（价格范围内）≥ 需求量 → 可全成交。
    // 市价单（极端价）覆盖全部对手盘；限价单按价格边界。
    return book.availableQty(side, price) >= quantity;
}

// ── 撮合核心 ──

void MatchingEngine::matchBuyOrder(uint64_t locate, OrderBook& book, Order& order, std::vector<BinaryResponse>& out)
{
    while (order.remaining_qty > 0)
    {
        Order* best_ask = book.getBestAsk(order.user_id);
        if (!best_ask) break;

        if (order.price < best_ask->price) break;

        uint32_t trade_qty = std::min(order.remaining_qty, best_ask->remaining_qty);

        order.remaining_qty -= trade_qty;
        order.filled_qty   += trade_qty;

        book.reduceOrderQty(best_ask, trade_qty);

        // 记录成交（交易回报：被吃的 resting 是 best_ask，方向 = 买单吃卖单）
        {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_TRADE;
            rsp.data.trade.locate    = locate;
            rsp.data.trade.order_ref = best_ask->order_id;
            rsp.data.trade.side      = SIDE_SELL;   // 被吃的是卖单
            rsp.data.trade.price     = best_ask->price;
            rsp.data.trade.quantity  = trade_qty;
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
                t.seq = next_sequence_.load();
            }
        }

        if (best_ask->remaining_qty == 0)
        {
            best_ask->status = OrderStatus::FILLED;
            // removeOrder 后 best_ask 悬空，循环顶部重新获取
            book.removeOrder(best_ask);
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

void MatchingEngine::matchSellOrder(uint64_t locate, OrderBook& book, Order& order, std::vector<BinaryResponse>& out)
{
    while (order.remaining_qty > 0)
    {
        Order* best_bid = book.getBestBid(order.user_id);
        if (!best_bid) break;

        if (order.price > best_bid->price) break;

        uint32_t trade_qty = std::min(order.remaining_qty, best_bid->remaining_qty);

        order.remaining_qty -= trade_qty;
        order.filled_qty   += trade_qty;

        book.reduceOrderQty(best_bid, trade_qty);

        // 记录成交（交易回报：被吃的 resting 是 best_bid，方向 = 卖单吃买单）
        {
            auto& rsp = out.emplace_back();
            rsp.type = RSP_TRADE;
            rsp.data.trade.locate    = locate;
            rsp.data.trade.order_ref = best_bid->order_id;
            rsp.data.trade.side      = SIDE_BUY;   // 被吃的是买单
            rsp.data.trade.price     = best_bid->price;
            rsp.data.trade.quantity  = trade_qty;
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
                t.seq = next_sequence_.load();
            }
        }

        if (best_bid->remaining_qty == 0)
        {
            best_bid->status = OrderStatus::FILLED;
            book.removeOrder(best_bid);
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
