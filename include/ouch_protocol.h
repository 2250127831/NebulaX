#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// ── OUCH 4.2 交易协议（撮合引擎侧，对照 NebulaX-Trader oms/ouch_order_codec.h）──
// Trader(客户) ↔ 撮合引擎(交易所) 的 TCP 全双工交易协议。定长大端、无长度前缀，
// 按首字节 type 分帧，每消息带 1B checksum（sum(前 N-1 字节) mod 256）。
//
// 入站（Trader → 撮合引擎）：
//   Enter Order 'O'(49B)：token(14 位定宽十进制 order_id) + side + shares
//     + orderBook(10B = symbol_id 数字=locate) + price(分×100) + tif('Y')
//     + display('N') + capacity('A') + isc('N') + minQty(0) + checksum
//   Cancel 'X'(19B)：token + shares(0=撤全部) + checksum
// 出站（撮合引擎 → Trader）：
//   Accepted 'A'(39B)：Order Reference Number(8B 交易所分配) + token + side
//     + shares + orderBook + checksum
//   Executed 'E'(42B)：ref + token + side + shares(原量) + price(订单价)
//     + executedQty(本次成交量) + executedPrice(成交价) + liquidity('T') + checksum
//   Canceled 'C'(39B)：ref + token + side + canceledQty + orderBook + checksum
//   Rejected 'J'(36B)：ref + token + side + reason + checksum
//
// 订单双标识：
//   Order Token = Trader 生成（order_id 定宽十进制），'O' 下单用，撮合引擎只回显。
//   Order Reference Number = 撮合引擎分配（u64 自增），'A' 带回，此后 E/C/J 以此关联。
//   orderBook = symbol_id 数字（= Stock Locate），非 symbol 文本。

// ── 消息类型 ──
constexpr uint8_t kOuchMsgOrder     = 'O';   // Enter Order（入站）
constexpr uint8_t kOuchMsgCancelReq = 'X';   // Cancel Order（入站）
constexpr uint8_t kOuchMsgBookReq   = 'Q';   // Book Query（入站，柜台转限价用）
constexpr uint8_t kOuchMsgAck       = 'A';   // Accepted（出站）
constexpr uint8_t kOuchMsgExec      = 'E';   // Executed（出站）
constexpr uint8_t kOuchMsgCancel    = 'C';   // Canceled（出站）
constexpr uint8_t kOuchMsgReject    = 'J';   // Rejected（出站）
constexpr uint8_t kOuchMsgBook      = 'B';   // Book（出站，盘口响应）

// ── 消息长度 ──
constexpr size_t kOuchOrderMsgLen   = 49;   // 'O' Enter Order
constexpr size_t kOuchCancelReqLen  = 19;   // 'X' Cancel Order
constexpr size_t kOuchBookReqLen    = 13;   // 'Q' Book Query：type + orderBook(10) + checksum
constexpr size_t kOuchAckMsgLen     = 39;   // 'A' Accepted
constexpr size_t kOuchExecMsgLen    = 42;   // 'E' Executed
constexpr size_t kOuchCancelMsgLen  = 39;   // 'C' Canceled
constexpr size_t kOuchRejectMsgLen  = 36;   // 'J' Rejected
constexpr size_t kOuchBookMsgLen    = 34;   // 'B' Book：type + orderBook(10) + bid(4) + bidVol(4) + ask(4) + askVol(4) + checksum

// 解码后的 Enter Order（撮合引擎侧）
struct OuchEnter {
    uint64_t order_id;     // token → order_id（14 位定宽十进制）
    uint8_t  side;         // 'B'/'S'
    uint32_t shares;       // 数量
    uint64_t symbol_id;    // orderBook 10B 数字 → symbol_id（= Stock Locate）
    uint64_t price;        // 分×100（decode 时 /100 → 分）
    char     tif;          // Time in Force（offset 34）：'Y'=市价IOC(兼容) 'D'=当日限价 'I'=IOC 'F'=FOK
};

// 解码 Enter Order（49B）。校验 type、长度、checksum。返回是否成功。
bool ouch_decode_enter(const uint8_t* buf, size_t len, OuchEnter& out);

// 解码 Book Query（13B）。返回 orderBook（locate 数字）。校验 type/checksum。
bool ouch_decode_book_req(const uint8_t* buf, size_t len, uint64_t& locate);

// 编码 Book 盘口响应（34B）。bid/ask 价格（分，encode ×100）+ 量。
size_t ouch_encode_book(uint64_t locate, uint32_t bid_price_cents, uint32_t bid_vol,
                        uint32_t ask_price_cents, uint32_t ask_vol, uint8_t* buf);

// 编码 Accepted（39B）。ref 由撮合引擎分配。
size_t ouch_encode_accepted(uint64_t ref, uint64_t order_id, char side,
                            uint32_t shares, uint64_t symbol_id, uint8_t* buf);

// 编码 Executed（42B）。本次成交量 + 成交价（分，encode ×100）。
size_t ouch_encode_executed(uint64_t ref, uint64_t order_id, char side,
                            uint32_t shares, uint64_t price_cents,
                            uint32_t exec_qty, uint64_t exec_price_cents, uint8_t* buf);

// 编码 Canceled（39B）。
size_t ouch_encode_canceled(uint64_t ref, uint64_t order_id, char side,
                            uint32_t canceled_qty, uint64_t symbol_id, uint8_t* buf);

// 编码 Rejected（36B）。
size_t ouch_encode_rejected(uint64_t ref, uint64_t order_id, char side,
                            const char* reason, uint8_t* buf);
