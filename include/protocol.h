#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>

// ── Binary command type constants ──
constexpr uint8_t CMD_NEW    = 0x01;
constexpr uint8_t CMD_CANCEL = 0x02;
constexpr uint8_t CMD_BOOK   = 0x03;

// ── Binary side constants ──
constexpr uint8_t SIDE_BUY  = 0x01;
constexpr uint8_t SIDE_SELL = 0x02;

// ── Binary command: 32 bytes, fixed size ──
//
// All fields in host byte order (little-endian on x86-64).
// Layout (natural alignment, no packing needed):
//   [0]    type        uint8_t   CMD_NEW / CMD_CANCEL / CMD_BOOK
//   [1]    side        uint8_t   SIDE_BUY / SIDE_SELL (NEW only)
//   [2-3]  _pad
//   [4-7]  price       uint32_t  price * 100 (NEW only)
//   [8-11] quantity    uint32_t  (NEW only)
//  [16-23] user_id     uint64_t  (NEW, CANCEL)
//  [24-31] order_id    uint64_t  (CANCEL only)
//
struct BinaryCommand
{
    uint8_t  type;
    uint8_t  side;
    uint8_t  _pad[2];
    uint32_t price;
    uint32_t quantity;
    uint64_t user_id;
    uint64_t order_id;
};
static_assert(sizeof(BinaryCommand) == 32, "BinaryCommand must be 32 bytes");

// ── Response type constants ──
constexpr uint8_t RSP_TRADE     = 0x81;
constexpr uint8_t RSP_OK        = 0x82;
constexpr uint8_t RSP_FILLED    = 0x83;
constexpr uint8_t RSP_CANCELLED = 0x84;
constexpr uint8_t RSP_ERROR     = 0x85;
constexpr uint8_t RSP_BOOK      = 0x86;
constexpr uint8_t RSP_HEADER   = 0x87;
constexpr uint8_t RSP_CLOSE    = 0x88;

// ── Error codes ──
enum class ErrorCode : uint16_t
{
    INVALID_SIDE           = 1,
    INVALID_PRICE_QTY_USER = 2,
    ORDER_NOT_FOUND        = 3,
    INVALID_COMMAND_TYPE   = 4,
    POOL_FULL              = 5,
};

// ── Binary response: 48 bytes, fixed size ──
//
// Layout (48 bytes):
//   [0]   type       uint8_t   discriminator
//   [1-3] _pad
//   [8-47] union               type-dependent payload
//
struct BinaryResponse
{
    uint8_t type;
    uint8_t _pad[3];

    union
    {
        // type == RSP_TRADE
        struct
        {
            uint32_t price;
            uint32_t quantity;
            uint64_t buyer_id;
            uint64_t seller_id;
            uint64_t buy_order_id;
            uint64_t sell_order_id;
        } trade;

        // type == RSP_OK / RSP_FILLED / RSP_CANCELLED
        struct
        {
            uint64_t order_id;
        } ack;

        // type == RSP_ERROR
        struct
        {
            uint16_t code;
        } error;

        // type == RSP_HEADER (线程间通信：fd + 后续响应帧数)
        // type == RSP_CLOSE 时 ack_ptr 指向关闭确认原子变量
        struct
        {
            int      client_fd;
            uint32_t count;
            std::atomic<bool>* ack_ptr;       // RSP_CLOSE: Send 线程 close(fd) 后置 true
        } header;

        // type == RSP_BOOK
        struct
        {
            uint32_t bid_price;
            uint32_t bid_volume;
            uint32_t ask_price;
            uint32_t ask_volume;
        } book;
    } data;
};
static_assert(sizeof(BinaryResponse) == 48, "BinaryResponse must be 48 bytes");

// ── Helpers ──

// Validate a decoded binary command.
// Returns true if the command is well-formed.
bool validateCommand(const BinaryCommand& cmd);
