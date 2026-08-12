#include "ouch_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
inline uint32_t rd_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
}
inline void wr_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}
inline void wr_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) p[7 - i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint64_t rd_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
// checksum = 前 n 字节和 mod 256
inline uint8_t checksum(const uint8_t* b, size_t n) {
    unsigned s = 0;
    for (size_t i = 0; i < n; ++i) s += b[i];
    return static_cast<uint8_t>(s & 0xFF);
}
// token = order_id 的 14 位定宽十进制（左补零），与 Trader codec 一致。
// 直接写进 dst（栈缓冲），零堆分配——交易回报路径不产生 std::string。
inline void put_token(char* dst, uint64_t order_id) {
    snprintf(dst, 15, "%014llu", (unsigned long long)order_id);
}
}  // namespace

bool ouch_decode_enter(const uint8_t* buf, size_t len, OuchEnter& out) {
    if (len < kOuchOrderMsgLen || buf[0] != kOuchMsgOrder) return false;
    if (buf[48] != checksum(buf, kOuchOrderMsgLen - 1)) return false;   // checksum 校验

    out = OuchEnter{};
    // Token（1-14）：14 位定宽十进制 order_id
    char token[15];
    std::memcpy(token, buf + 1, 14); token[14] = '\0';
    out.order_id = strtoull(token, nullptr, 10);
    out.side     = buf[15];
    out.shares   = rd_be32(buf + 16);
    // orderBook（20-29）：symbol_id 数字，左对齐 10B 空补空格 → strtoull
    char book[11];
    std::memcpy(book, buf + 20, 10); book[10] = '\0';
    out.symbol_id = strtoull(book, nullptr, 10);
    // price（30-33）：分×100 → 分
    out.price = rd_be32(buf + 30) / 100;
    // Time in Force（34）：'Y'=市价IOC(兼容) 'D'=当日限价 'I'=IOC 'F'=FOK
    out.tif = (char)buf[34];
    return true;
}

bool ouch_decode_book_req(const uint8_t* buf, size_t len, uint64_t& locate) {
    if (len < kOuchBookReqLen || buf[0] != kOuchMsgBookReq) return false;
    if (buf[12] != checksum(buf, kOuchBookReqLen - 1)) return false;
    char book[11];
    std::memcpy(book, buf + 1, 10); book[10] = '\0';
    locate = strtoull(book, nullptr, 10);
    return true;
}

size_t ouch_encode_book(uint64_t locate, uint32_t bid_price_cents, uint32_t bid_vol,
                        uint32_t ask_price_cents, uint32_t ask_vol, uint8_t* buf) {
    // 'B'(1) + orderBook(10) + bid(4) + bidVol(4) + ask(4) + askVol(4) + checksum(1) = 34B
    std::memset(buf, ' ', kOuchBookMsgLen);
    buf[0] = kOuchMsgBook;
    char book[11];
    snprintf(book, sizeof(book), "%llu", (unsigned long long)locate);
    std::memcpy(buf + 1, book, strlen(book));
    wr_be32(buf + 11, bid_price_cents * 100);
    wr_be32(buf + 15, bid_vol);
    wr_be32(buf + 19, ask_price_cents * 100);
    wr_be32(buf + 23, ask_vol);
    buf[33] = checksum(buf, kOuchBookMsgLen - 1);
    return kOuchBookMsgLen;
}

size_t ouch_encode_accepted(uint64_t ref, uint64_t order_id, char side,
                            uint32_t shares, uint64_t symbol_id, uint8_t* buf) {
    // 'A'(1) + ref(8) + token(14) + side(1) + shares(4) + orderBook(10) + checksum(1) = 39B
    std::memset(buf, ' ', kOuchAckMsgLen);
    buf[0] = kOuchMsgAck;
    wr_be64(buf + 1, ref);
    put_token(reinterpret_cast<char*>(buf + 9), order_id);
    buf[23] = side;
    wr_be32(buf + 24, shares);
    char book[11];
    snprintf(book, sizeof(book), "%llu", (unsigned long long)symbol_id);
    std::memcpy(buf + 28, book, strlen(book));
    buf[38] = checksum(buf, kOuchAckMsgLen - 1);
    return kOuchAckMsgLen;
}

size_t ouch_encode_executed(uint64_t ref, uint64_t order_id, char side,
                            uint32_t shares, uint64_t price_cents,
                            uint32_t exec_qty, uint64_t exec_price_cents, uint8_t* buf) {
    // 'E'(1) + ref(8) + token(14) + side(1) + shares(4) + price(4)
    // + executedQty(4) + executedPrice(4) + liquidity(1) + checksum(1) = 42B
    std::memset(buf, ' ', kOuchExecMsgLen);
    buf[0] = kOuchMsgExec;
    wr_be64(buf + 1, ref);
    put_token(reinterpret_cast<char*>(buf + 9), order_id);
    buf[23] = side;
    wr_be32(buf + 24, shares);                  // 订单原量
    wr_be32(buf + 28, static_cast<uint32_t>(price_cents * 100));       // 订单价（分×100）
    wr_be32(buf + 32, exec_qty);                // 本次成交量
    wr_be32(buf + 36, static_cast<uint32_t>(exec_price_cents * 100));  // 成交价（分×100）
    buf[40] = 'T';                              // liquidity: taker
    buf[41] = checksum(buf, kOuchExecMsgLen - 1);
    return kOuchExecMsgLen;
}

size_t ouch_encode_canceled(uint64_t ref, uint64_t order_id, char side,
                            uint32_t canceled_qty, uint64_t symbol_id, uint8_t* buf) {
    // 'C'(1) + ref(8) + token(14) + side(1) + canceledQty(4) + orderBook(10) + checksum(1) = 39B
    std::memset(buf, ' ', kOuchCancelMsgLen);
    buf[0] = kOuchMsgCancel;
    wr_be64(buf + 1, ref);
    put_token(reinterpret_cast<char*>(buf + 9), order_id);
    buf[23] = side;
    wr_be32(buf + 24, canceled_qty);
    char book[11];
    snprintf(book, sizeof(book), "%llu", (unsigned long long)symbol_id);
    std::memcpy(buf + 28, book, strlen(book));
    buf[38] = checksum(buf, kOuchCancelMsgLen - 1);
    return kOuchCancelMsgLen;
}

size_t ouch_encode_rejected(uint64_t ref, uint64_t order_id, char side,
                            const char* reason, uint8_t* buf) {
    // 'J'(1) + ref(8) + token(14) + side(1) + reason(11) + checksum(1) = 36B
    std::memset(buf, ' ', kOuchRejectMsgLen);
    buf[0] = kOuchMsgReject;
    wr_be64(buf + 1, ref);
    put_token(reinterpret_cast<char*>(buf + 9), order_id);
    buf[23] = side;
    if (reason) {
        size_t i = 0;
        while (i < 11 && reason[i]) { buf[24 + i] = reason[i]; ++i; }
    }
    buf[35] = checksum(buf, kOuchRejectMsgLen - 1);
    return kOuchRejectMsgLen;
}
