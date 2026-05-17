#pragma once

#include <string>

#include "matching_engine.h"

class TcpServer
{
public:
    // 监听端口
    explicit TcpServer(
        int port,

        // 撮合引擎引用
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

    // 处理客户端请求
    std::string processRequest(
        const std::string& request
    );

private:
    // 监听端口号
    int port_ = 0;

    // server socket fd
    int server_fd_ = -1;

    // 撮合引擎引用
    // 网络层不拥有 engine 生命周期
    MatchingEngine& engine_;
};