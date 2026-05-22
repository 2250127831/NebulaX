#pragma once

#include <cstring>
#include <vector>
#include <unordered_map>

#include "spsc_byte_ring.h"
#include "matching_engine.h"
#include "protocol.h"

constexpr size_t RING_SIZE = 1048576;  // 1 MB

// 连接上下文：读缓冲（仅 IO+Matching 线程使用）
struct ConnContext
{
    int fd;
    static constexpr size_t BUF_SIZE = 4096;
    char read_buf[BUF_SIZE];
    size_t pending = 0;    // 缓冲区中有效字节数
    size_t consumed = 0;   // 已解析的字节偏移

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

    // epoll 事件循环（仅处理 EPOLLIN）
    void start();

private:
    void handleAccept();
    void handleRead(ConnContext* conn);
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
    int epoll_fd_ = -1;
    MatchingEngine& engine_;
    SPSCByteRing<RING_SIZE>& ring_;
    int wake_fd_ = -1;

    static constexpr int MAX_EVENTS = 64;
    std::unordered_map<int, ConnContext*> conns_;
};
