#include "protocol.h"
#include <sstream>
#include <cctype>

// 将字符串转为大写用于比较命令
static std::string toUpper(const std::string& s)
{
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

Command Protocol::parseCommand(const std::string& message)
{
    Command cmd;
    std::istringstream iss(message);
    std::string token;
    if (!(iss >> token))
    {
        // 空消息
        cmd.type = CommandType::INVALID;
        return cmd;
    }

    std::string cmdType = toUpper(token);

    if (cmdType == "NEW")
    {
        cmd.type = CommandType::NEW;
        std::string sideStr;
        uint32_t price = 0, qty = 0;
        uint64_t user_id=0;
        if (!(iss >> sideStr >> price >> qty >> user_id))
        {
            // 参数不足
            cmd.type = CommandType::INVALID;
            return cmd;
        }
        std::string sideUpper = toUpper(sideStr);
        if (sideUpper == "BUY")
        {
            cmd.side = Side::BUY;
        }
        else if (sideUpper == "SELL")
        {
            cmd.side = Side::SELL;
        }
        else
        {
            // 无效方向
            cmd.type = CommandType::INVALID;
            cmd.side = Side::INVALID;
            return cmd;
        }
        cmd.price = price;
        cmd.quantity = qty;
        cmd.user_id = user_id;
        // 检查价格和数量和用户id是否有效（>0）
        if (price == 0 || qty == 0 || user_id == 0)
        {
            cmd.type = CommandType::INVALID;
            return cmd;
        }
        // 检查是否有多余参数（可忽略，也可严格报错，这里宽松忽略）
    }
    else if (cmdType == "CANCEL")
    {
        cmd.type = CommandType::CANCEL;
        uint64_t oid = 0, user_id = 0;
        if (!(iss >> oid >> user_id))
        {
            cmd.type = CommandType::INVALID;
            return cmd;
        }
        cmd.order_id = oid;
        cmd.user_id = user_id;
        // 检查是否有多余参数（忽略）
    }
    else if (cmdType == "BOOK")
    {
        cmd.type = CommandType::BOOK;
        // 尝试读取盘口档数，缺省为 5
        if (!(iss >> cmd.levels))
        {
            cmd.levels = 5; // 无参数时默认 5 档
        }
        // 如果有多余参数也无所谓，直接忽略
    }
    else
    {
        cmd.type = CommandType::INVALID;
    }

    return cmd;
}