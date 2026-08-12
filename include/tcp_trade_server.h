#pragma once

#include "matching_engine.h"
#include "protocol.h"
#include "ouch_protocol.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

// ── TCP 全双工交易服务器（OUCH 4.2，对照 Trader ouch_order_codec.h）──
// 撮合引擎作为模拟交易所：监听 trade_port，accept Trader 单连接，
// 同一连接收 OUCH Enter/Cancel → 撮合引擎撮合 → 回 Accepted/Executed/Canceled/Rejected。
// 独立线程运行，与 TcpServer（ITCH 输入）并行。engine 调用带锁。
//
// 订单双标识：
//   Order Token = Trader 的 order_id（14 位定宽十进制），decode 'O' 得 order_id。
//   Order Reference Number = 撮合引擎分配（ref_for(order_id) 自增），'A' 带回，
//     E/C/J 取同一 ref（与 Trader codec 的 id_to_ref_ 语义一致）。
//   orderBook = symbol_id 数字（= Stock Locate），直接作引擎 locate。
// 订单类型：本期 Trader 只发市价单（MARKET，IOC），成交按盘口价，无对手盘回 E qty=0。
class TcpTradeServer {
public:
    TcpTradeServer(uint16_t port, MatchingEngine& engine,
                   std::mutex& engine_mtx);
    ~TcpTradeServer();

    TcpTradeServer(const TcpTradeServer&) = delete;
    TcpTradeServer& operator=(const TcpTradeServer&) = delete;

    // 启动监听 + accept 循环（阻塞，运行在独立线程）
    void start();

    // 停止并释放（打断阻塞的 accept/recv）
    void stop();

    bool ok() const { return server_fd_ >= 0; }

private:
    // 处理一个已连接的客户（收 OUCH → 撮合 → 回 OUCH）
    void handleClient(int fd);

    // 处理一条 OUCH Enter Order（'O' 49B）
    void handleEnter(int fd, const OuchEnter& ord);
    // 处理一条 OUCH Cancel Order（'X' 19B，token + shares）
    void handleCancel(int fd, uint64_t order_id, uint32_t shares);

    // 取订单的交易所 Order Reference Number（未分配则分配，与 Trader id_to_ref_ 一致）
    uint64_t ref_for(uint64_t order_id);

    uint16_t port_;
    MatchingEngine& engine_;
    std::mutex& engine_mtx_;           // 共享引擎调用锁（与 TcpServer 并存）
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;

    // order_id → ref（撮合引擎分配，E/C/J 同一 ref）
    std::unordered_map<uint64_t, uint64_t> id_to_ref_;
    // order_id → locate（撤单需要路由到簿）
    std::unordered_map<uint64_t, uint64_t> order_locate_;
    uint64_t next_ref_ = 1;            // Order Reference Number 分配器
};
