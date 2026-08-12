#include "itch_parser.h"

#include <cstring>
#include <string>

namespace {
// 大端序读取辅助（迁移自 NebulaX-Trader itch_parser.cpp）
inline uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t rd_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
}
inline uint64_t rd_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
}  // namespace

bool ItchParser::feed(const uint8_t* m, size_t len, ItchEvent& out) {
    if (len < 3) return false;   // 至少 type(1) + 少量字段
    const uint8_t mt = m[0];

    // R 消息（Stock Directory）：建立 symbol→locate 映射，不产生订单事件。
    // 布局：type(1) locate(2) track(2) ts(6) symbol(8) ...
    if (mt == 'R') {
        if (len < 19) return false;
        uint16_t loc = rd_u16(m + 1);
        // symbol 8B，去尾空白
        char sym[9] = {};
        for (int i = 0; i < 8; ++i) sym[i] = (char)m[11 + i];
        std::string s(sym);
        // 去掉尾部空格/空字符
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
        if (!s.empty()) symbol_map_[s] = loc;
        return false;   // R 不是订单事件
    }

    switch (mt) {
        case 'A': case 'F': {   // Add Order（36B / 40B）
            if (len < 36) return false;
            out.type       = ItchEvent::Type::ADD;
            out.locate     = rd_u16(m + 1);
            out.order_ref  = rd_u64(m + 11);
            out.side       = (m[19] == 'S') ? Side::SELL : Side::BUY;
            out.shares     = rd_u32(m + 20);
            out.price      = itch_to_cents(rd_u32(m + 32));
            return true;
        }
        case 'D': {   // Order Delete（19B）
            if (len < 19) return false;
            out.type       = ItchEvent::Type::DELETE;
            out.locate     = rd_u16(m + 1);
            out.order_ref  = rd_u64(m + 11);
            out.side       = Side::INVALID;
            return true;
        }
        case 'X': {   // Order Cancel（23B）
            if (len < 23) return false;
            out.type       = ItchEvent::Type::CANCEL;
            out.locate     = rd_u16(m + 1);
            out.order_ref  = rd_u64(m + 11);
            out.shares     = rd_u32(m + 19);
            out.side       = Side::INVALID;
            return true;
        }
        case 'U': {   // Order Replace（35B）
            if (len < 35) return false;
            out.type         = ItchEvent::Type::REPLACE;
            out.locate       = rd_u16(m + 1);
            out.order_ref    = rd_u64(m + 11);
            out.new_order_ref= rd_u64(m + 19);
            out.shares       = rd_u32(m + 27);
            out.price        = itch_to_cents(rd_u32(m + 31));
            out.side         = Side::INVALID;   // U 无方向，由调用方查旧单
            return true;
        }
        case 'Q': {   // Book Query（模拟交易所扩展）：[1]'Q' [2]locate
            if (len < 3) return false;
            out.type   = ItchEvent::Type::BOOK;
            out.locate = rd_u16(m + 1);
            return true;
        }
        default:
            // P/E/C（成交）是引擎输出，R/S/H/Y/L 等系统消息非订单事件 → 不产生输入
            return false;
    }
}
