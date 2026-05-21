#pragma once

#include <cstring>
#include <vector>
#include <unordered_map>

#include "matching_engine.h"
#include "protocol.h"

// 连接上下文：读缓冲 + 待发送响应 + EPOLLOUT 状态
struct ConnContext
{
    int fd;
    static constexpr size_t BUF_SIZE = 4096;
    char read_buf[BUF_SIZE];
    size_t pending = 0;    // 缓冲区中有效字节数
    size_t consumed = 0;   // 已解析的字节偏移
    std::vector<BinaryResponse> resp_buf;
    size_t resp_sent = 0;  // resp_buf[0] 已发送的字节数（可能 < 48）
    bool write_interested = false;

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
        MatchingEngine& engine
    );

    ~TcpServer();

    // epoll 事件循环
    void start();

private:
    // epoll 事件处理
    void handleAccept();
    void handleRead(ConnContext* conn);
    void handleWrite(ConnContext* conn);
    void trySendResponses(ConnContext* conn);
    void closeConnection(ConnContext* conn);

    // 处理二进制命令，往 out 追加响应帧
    void processRequest(
        const BinaryCommand& cmd,
        std::vector<BinaryResponse>& out_responses
    );

private:
    int port_ = 0;
    int server_fd_ = -1;
    int epoll_fd_ = -1;
    MatchingEngine& engine_;

    static constexpr int MAX_EVENTS = 64;
    std::unordered_map<int, ConnContext*> conns_;
};
