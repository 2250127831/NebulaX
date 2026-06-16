#pragma once

#include <cstdint>

// IO+Matching 线程计数器（主线程写，Send 线程不碰）
struct IOCounters
{
    uint64_t recv_frames     = 0;
    uint64_t new_orders      = 0;
    uint64_t cancels         = 0;
    uint64_t book_queries    = 0;
    uint64_t trades          = 0;
    uint64_t errors          = 0;
    uint64_t order_pool_used      = 0;
    uint64_t order_pool_capacity  = 0;  // 启动时设一次，不变
};

// Send 线程计数器（Send 线程写，IO 线程不碰）
struct SendCounters
{
    uint64_t send_batches    = 0;
    uint64_t send_bytes      = 0;
    uint64_t send_zc_ok      = 0;
    uint64_t send_zc_fail    = 0;
};

// 共享内存布局（总大小 14 × uint64_t = 112 bytes）
// 外部采集器直接按 14 个 uint64_t 顺序解析：
//   [0]  io_thread_pid
//   [1]  send_thread_pid
//   [2-9]   IOCounters (8 counters)
//   [10-13] SendCounters (4 counters)
struct SharedMetrics
{
    uint64_t io_thread_pid   = 0;   // [0]
    uint64_t send_thread_pid = 0;   // [1]
    IOCounters io;                  // [2-9]
    SendCounters send;              // [10-13]
};
static_assert(sizeof(SharedMetrics) == 14 * sizeof(uint64_t),
              "SharedMetrics layout mismatch");
