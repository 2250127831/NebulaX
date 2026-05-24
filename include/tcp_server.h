#pragma once

#include <cstring>
#include <vector>
#include <unordered_map>

#include "spsc_byte_ring.h"
#include "io_uring_poller.h"
#include "matching_engine.h"
#include "protocol.h"

constexpr size_t RING_SIZE = 1048576;  // 1 MB

// 连接上下文：读缓冲（仅 IO+Matching 线程使用）
struct ConnContext
{
    int fd;
    char* read_buf = nullptr;  // 指向 io_uring 固定缓冲区
    uint32_t buf_idx = UINT32_MAX;  // 固定缓冲区索引，用于 re-arm recv
    size_t pending = 0;
    size_t consumed = 0;

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
        int wake_fd
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
    void pushResponses(int fd, const std::vector<BinaryResponse>& buf);
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
};
