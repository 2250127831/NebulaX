#pragma once

#include <string>
#include <vector>

#include "order.h"

// 协议命令类型
enum class CommandType : uint8_t
{
    INVALID,   // 非法命令

    NEW,       // 新订单
    CANCEL,    // 撤单
    BOOK        // 查看盘口
};

// 客户端请求命令
struct Command
{
    // 命令类型
    CommandType type =
        CommandType::INVALID;

    // 买 / 卖
    // 仅 NEW 有效
    Side side =
        Side::INVALID;

    // 价格
    // 仅 NEW 有效
    uint32_t price = 0;

    // 数量
    // 仅 NEW 有效
    uint32_t quantity = 0;

    // 撤单 id
    // 仅 CANCEL 有效
    uint64_t order_id = 0;


    // 数量
    // 仅 NEW 和 CANCEL 有效
    uint64_t user_id = 0;

    // 盘口档数
    // 仅 BOOK 有效
    int levels = 5;
};

class Protocol
{
public:
    // 解析客户端请求
    //
    // 例如:
    // "NEW BUY 10120 50 1"
    //
    // 返回解析后的 Command
    static Command parseCommand(
        const std::string& message
    );
};