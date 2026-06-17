#pragma once

#include <cstring>
#include <vector>
#include <list>
#include <unordered_map>
#include <atomic>

#include "spsc_byte_ring.h"
#include "io_uring_poller.h"
#include "matching_engine.h"
#include "protocol.h"
#include "metrics.h"

// 连接上下文：读缓冲（仅 IO+Matching 线程使用）
struct ConnContext
{
    int fd;
    char* read_buf = nullptr;  // 指向 io_uring 固定缓冲区
    uint32_t buf_idx = UINT32_MAX;  // 固定缓冲区索引，用于 re-arm recv
    size_t pending = 0;
    size_t consumed = 0;

    bool closing = false;                      // 正在关闭，不再提交 recv
    std::atomic<bool> close_acked{false};      // Send 线程 close(fd) 后置 true

    void compact()
    {
        size_t remaining = pending - consumed;
        if (remaining > 0 && remaining < pending)
            memmove(read_buf, read_buf + consumed, remaining);
        pending = remaining;
        consumed = 0;
    }
};

class TcpServer
{
public:
    explicit TcpServer(
        int port,
        MatchingEngine& engine,
        SPSCByteRing<RING_SIZE>& resp_ring,
        int wake_fd,
        IOCounters* metrics,
        uint64_t* io_heartbeat = nullptr,
        uint64_t* send_heartbeat = nullptr
    );

    ~TcpServer();

    // io_uring 事件循环
    void start();

private:
    // 新连接到达（由 io_uring accept CQE 触发）
    void onAccept(int client_fd);

    // recv 完成：bytes_read 是从 io_uring CQE 获取的读取字节数
    void onRecv(ConnContext* conn, int bytes_read);

    void closeConnection(ConnContext* conn);
    void drainPendingClose();
    void logSummary();
    // 推送响应，返回 false 表示连接已断开（调用方应 closeConnection）
    bool pushResponses(int fd, const std::vector<BinaryResponse>& buf);
    void notifySendThread();

    void processRequest(
        const BinaryCommand& cmd,
        std::vector<BinaryResponse>& out_responses
    );

private:
    int port_ = 0;
    int server_fd_ = -1;
    MatchingEngine& engine_;
    SPSCByteRing<RING_SIZE>& ring_;
    int wake_fd_ = -1;

    IoUringPoller poller_;
    std::unordered_map<int, ConnContext*> conns_;
    std::list<ConnContext*> pending_closes_;  // 等待 Send 确认的关闭连接（尾插，老的在队首）
    IOCounters* metrics_ = nullptr;     // 指向 shared.io
    uint64_t*   io_heartbeat_ = nullptr;
    uint64_t*   send_heartbeat_ = nullptr;
    uint64_t summary_last_orders_ = 0;
};
