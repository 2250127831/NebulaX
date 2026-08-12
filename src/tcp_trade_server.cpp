#include "tcp_trade_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <cstring>
#include <vector>

TcpTradeServer::TcpTradeServer(uint16_t port, MatchingEngine& engine,
                               std::mutex& engine_mtx)
    : port_(port), engine_(engine), engine_mtx_(engine_mtx) {}

TcpTradeServer::~TcpTradeServer() {
    stop();
}

void TcpTradeServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_fd_ < 0) return;

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }
    if (listen(server_fd_, 4) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() {
        while (running_.load(std::memory_order_acquire)) {
            sockaddr_in cli{};
            socklen_t clen = sizeof(cli);
            int fd = accept(server_fd_, (sockaddr*)&cli, &clen);
            if (fd < 0) {
                usleep(1000);
                continue;
            }
            handleClient(fd);
        }
    });
}

uint64_t TcpTradeServer::ref_for(uint64_t order_id) {
    auto it = id_to_ref_.find(order_id);
    if (it != id_to_ref_.end()) return it->second;
    uint64_t ref = next_ref_++;
    id_to_ref_[order_id] = ref;
    return ref;
}

void TcpTradeServer::handleClient(int fd) {
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);   // 阻塞 recv

    uint8_t buf[256];
    while (running_.load(std::memory_order_acquire)) {
        // 读首字节 type → 按定长分帧（'O' 49 / 'X' 19 / 'Q' 13）
        ssize_t first = ::recv(fd, buf, 1, 0);
        if (first <= 0) break;   // 连接断/超时
        size_t mlen = 0;
        if (buf[0] == kOuchMsgOrder) mlen = kOuchOrderMsgLen;
        else if (buf[0] == kOuchMsgCancelReq) mlen = kOuchCancelReqLen;
        else if (buf[0] == kOuchMsgBookReq) mlen = kOuchBookReqLen;
        else break;   // 未知消息类型，断开

        size_t got = 1;
        while (got < mlen) {
            ssize_t n = ::recv(fd, buf + got, mlen - got, 0);
            if (n <= 0) { ::close(fd); return; }
            got += static_cast<size_t>(n);
        }

        if (buf[0] == kOuchMsgOrder) {
            OuchEnter ord;
            if (ouch_decode_enter(buf, got, ord)) {
                handleEnter(fd, ord);
            }
        } else if (buf[0] == kOuchMsgBookReq) {
            uint64_t locate = 0;
            if (ouch_decode_book_req(buf, got, locate)) {
                // 盘口查询：回 'B' 当前 best bid/ask（柜台转限价用）
                BinaryResponse br;
                {
                    std::lock_guard<std::mutex> lk(engine_mtx_);
                    engine_.getBook(locate, br);
                }
                uint8_t bb[kOuchBookMsgLen];
                size_t blen = ouch_encode_book(locate, br.data.book.bid_price,
                                               br.data.book.bid_volume,
                                               br.data.book.ask_price,
                                               br.data.book.ask_volume, bb);
                send(fd, bb, blen, MSG_NOSIGNAL);
            }
        } else {   // 'X' Cancel
            char token[15];
            std::memcpy(token, buf + 1, 14); token[14] = '\0';
            uint64_t order_id = strtoull(token, nullptr, 10);
            handleCancel(fd, order_id, 0);
        }
    }
    ::close(fd);
}

void TcpTradeServer::handleEnter(int fd, const OuchEnter& ord) {
    // 撮合引擎分配 Order Reference Number
    uint64_t ref = ref_for(ord.order_id);

    // symbol_id（orderBook 数字）= Stock Locate，直接作引擎 locate
    uint64_t locate = ord.symbol_id;
    order_locate_[ord.order_id] = locate;   // 撤单路由用
    Side side = (ord.side == 'S') ? Side::SELL : Side::BUY;
    uint32_t price_cents = static_cast<uint32_t>(ord.price);

    // TIF → 引擎成交条件：'Y'=市价IOC(兼容Trader现状) 'D'=当日限价 'I'=IOC 'F'=FOK
    OrderTif tif;
    switch (ord.tif) {
        case 'D':  tif = OrderTif::DAY; break;
        case 'I':  tif = OrderTif::IOC; break;
        case 'F':  tif = OrderTif::FOK; break;
        default:   tif = OrderTif::IOC; break;   // 'Y' 及其它 → IOC（兼容）
    }

    std::vector<BinaryResponse> out;
    {
        std::lock_guard<std::mutex> lk(engine_mtx_);
        engine_.processAdd(locate, ord.order_id, side, price_cents,
                           ord.shares, ord.order_id, out, tif);
    }

    // ── 回报（按 TIF 语义）──
    uint8_t buf[64];

    // 1. FOK 不能全成交 → 回 'J' Rejected（FOK 语义：整个订单作废）
    //    市价 IOC 无对手盘 → 回 'A' + 'E' qty=0（Trader 文档语义，不撤连接）
    if (tif == OrderTif::FOK) {
        for (auto& r : out) {
            if (r.type == RSP_ERROR &&
                r.data.error.code == static_cast<uint16_t>(ErrorCode::FOK_NO_FULL_FILL)) {
                size_t rlen = ouch_encode_rejected(ref, ord.order_id, ord.side,
                                                   "FOKNOFILL", buf);
                send(fd, buf, rlen, MSG_NOSIGNAL);
                return;
            }
        }
    }

    // 2. 'A' Accepted（受理确认；限价挂簿/市价立即成交都先受理）
    size_t ack_len = ouch_encode_accepted(ref, ord.order_id, ord.side,
                                          ord.shares, locate, buf);
    send(fd, buf, ack_len, MSG_NOSIGNAL);

    // 3. 成交回报 'E'（每笔成交一个；限价挂簿无成交则无 'E'）
    bool had_trade = false;
    for (auto& r : out) {
        if (r.type == RSP_TRADE) {
            had_trade = true;
            size_t elen = ouch_encode_executed(ref, ord.order_id, ord.side,
                                               ord.shares, price_cents,
                                               r.data.trade.quantity,
                                               r.data.trade.price, buf);
            send(fd, buf, elen, MSG_NOSIGNAL);
        }
    }
    // 市价单（IOC/FOK 兼容）零成交 → 回 'E' executedQty=0（Trader 文档：无对手盘不撤连接）
    if (!had_trade && tif != OrderTif::DAY) {
        size_t elen = ouch_encode_executed(ref, ord.order_id, ord.side,
                                           ord.shares, price_cents, 0, 0, buf);
        send(fd, buf, elen, MSG_NOSIGNAL);
    }
}

void TcpTradeServer::handleCancel(int fd, uint64_t order_id, uint32_t shares) {
    uint64_t ref = ref_for(order_id);
    // 撤单需路由到订单所在簿（locate）
    auto lit = order_locate_.find(order_id);
    if (lit == order_locate_.end()) return;   // 未知订单，静默忽略
    uint64_t locate = lit->second;
    std::vector<BinaryResponse> out;
    {
        std::lock_guard<std::mutex> lk(engine_mtx_);
        engine_.processCancel(locate, order_id, order_id, out);
    }
    // 撤单回报 'C'（Canceled）
    for (auto& r : out) {
        if (r.type == RSP_CANCELLED) {
            uint8_t buf[kOuchCancelMsgLen];
            size_t clen = ouch_encode_canceled(ref, order_id, 'B', 0, locate, buf);
            send(fd, buf, clen, MSG_NOSIGNAL);
        }
    }
}

void TcpTradeServer::stop() {
    running_.store(false, std::memory_order_release);
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}
