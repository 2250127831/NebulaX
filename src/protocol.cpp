#include "protocol.h"

bool validateCommand(const BinaryCommand& cmd)
{
    switch (cmd.type)
    {
        case CMD_NEW:
            return (cmd.side == SIDE_BUY || cmd.side == SIDE_SELL)
                && cmd.price  > 0
                && cmd.quantity > 0
                && cmd.user_id  > 0;

        case CMD_CANCEL:
            return cmd.order_id > 0
                && cmd.user_id  > 0;

        case CMD_BOOK:
            return true;

        default:
            return false;
    }
}
