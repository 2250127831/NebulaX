#pragma once

#include <vector>

#include "matching_engine.h"
#include "protocol.h"

class TcpServer
{
public:
    explicit TcpServer(
        int port,
        MatchingEngine& engine
    );

    ~TcpServer();

    // 启动服务器
    void start();

private:
    // 处理客户端连接
    void handleClient(
        int client_fd
    );

    // 处理二进制命令，往 out 追加响应帧
    void processRequest(
        const BinaryCommand& cmd,
        std::vector<BinaryResponse>& out_responses
    );

private:
    int port_ = 0;
    int server_fd_ = -1;
    MatchingEngine& engine_;
};
