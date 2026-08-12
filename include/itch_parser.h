#pragma once

#include "order.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>

// ── ITCH 5.0 解析器（迁移自 NebulaX-Trader market/parser/itch_parser）──
// 只做"字节 → 事件"的纯转换，不碰订单簿。解析出的订单事件喂 MatchingEngine。
//
// ITCH 消息帧格式（NASDAQ TotalView，大端序）：
//   每条消息以 2 字节 big-endian 长度前缀开头，length = 消息体字节数（不含前缀）。
//   消息体第一字节是消息类型（ASCII）。
//   价格：int32 × 1/10000 美元 → 内部统一转 uint32 分（itch_to_cents = /100）。
//
// 撮合引擎只消费订单事件（A/F/D/X/U），成交消息（P/E/C）是引擎输出、不作为输入。
// R 消息（Stock Directory）建立 symbol→locate 映射，供 OUCH 下单按 symbol 路由。

// 协议无关的订单事件（撮合引擎输入）
struct ItchEvent {
    enum class Type : uint8_t {
        ADD,        // A/F: 新挂单
        DELETE,     // D: 整笔撤单
        CANCEL,     // X: 部分撤单
        REPLACE,    // U: 改单
        BOOK,       // Q: 盘口查询（模拟交易所扩展，非 ITCH 标准消息）
    };

    Type       type;
    uint16_t   locate;       // Stock Locate（2 字节，0-65535，symbol 路由键）
    uint64_t   order_ref;    // 挂单引用（A/F/D/X 用）
    uint64_t   new_order_ref;// REPLACE 的新 ref（其他为 0）
    Side       side;         // ADD 有；D/X/U 未知填 INVALID
    uint32_t   price;        // 挂单价（分，ITCH /100 换算）
    uint32_t   shares;       // ADD/REPLACE 总量 / CANCEL 撤量
    uint64_t   user_id;      // ITCH 无账户归属，调用方填（默认 0 = 无归属）
};

class ItchParser {
public:
    // 喂一条消息体（不含长度前缀，len = 消息体长度）。
    // 返回 true = 解析为订单事件（out 填充）；false = 非订单消息/系统消息。
    // 非 const：R 消息（Stock Directory）更新 symbol→locate 映射表。
    bool feed(const uint8_t* msg, size_t len, ItchEvent& out);

    // symbol → locate 映射（R 消息建立）。OUCH 下单按 symbol 路由用。
    const std::unordered_map<std::string, uint16_t>& symbol_map() const {
        return symbol_map_;
    }

    // 用 symbol 反查 locate（不存在返回 UINT16_MAX）
    uint16_t locate_for_symbol(const std::string& sym) const {
        auto it = symbol_map_.find(sym);
        return (it != symbol_map_.end()) ? it->second : UINT16_MAX;
    }

private:
    static uint32_t itch_to_cents(uint32_t itch_price) {
        // ITCH: int32 × 1/10000 美元 → 分（1 美元 = 100 分 = 10000 ticks）
        return itch_price / 100;
    }

    std::unordered_map<std::string, uint16_t> symbol_map_;   // symbol → locate
};
