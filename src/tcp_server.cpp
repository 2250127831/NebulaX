#include "tcp_server.h"
#include "protocol.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>

TcpServer::TcpServer(int port, MatchingEngine& engine)
    : port_(port), engine_(engine)
{
    // 创建 TCP socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "ERROR: socket() failed\n";
        return;
    }

    // 允许地址重用，避免 TIME_WAIT 问题
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定地址和端口
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "ERROR: bind() failed\n";
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    // 开始监听
    if (listen(server_fd_, 10) < 0) {
        std::cerr << "ERROR: listen() failed\n";
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    std::cout << "NebulaX server listening on port " << port_ << "\n";
}

TcpServer::~TcpServer()
{
    if (server_fd_ != -1) {
        close(server_fd_);
    }
}

void TcpServer::start()
{
    if (server_fd_ < 0) {
        std::cerr << "ERROR: server not initialized\n";
        return;
    }

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            std::cerr << "ERROR: accept() failed\n";
            continue;
        }

        // 每个客户端连接交给 handleClient 处理
        handleClient(client_fd);

        // 处理完毕后关闭客户端连接
        close(client_fd);
    }
}

void TcpServer::handleClient(int client_fd)
{
    std::string request_line;
    char buffer[4096];
    ssize_t n;
    // 循环接收数据，按行处理（每行一个命令）
    while ((n = recv(client_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[n] = '\0';
        request_line += buffer;
        // 处理所有完整的行（以 '\n' 结尾）
        size_t pos;
        while ((pos = request_line.find('\n')) != std::string::npos) {
            std::string line = request_line.substr(0, pos);
            // 移除可能的 '\r'
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            request_line.erase(0, pos + 1);

            // 处理该行命令
            std::string response = processRequest(line);
            response += "\n";
            send(client_fd, response.c_str(), response.size(), 0);
        }
    }
    // 客户端断开或出错
}

std::string TcpServer::processRequest(const std::string& request)
{
    // 忽略空行
    if (request.empty())
        return "ERROR empty_request";

    // 解析协议命令
    Command cmd = Protocol::parseCommand(request);

    // 根据命令类型分发到撮合引擎
    switch (cmd.type) {
        case CommandType::NEW:
            return engine_.processNewOrder(cmd.side, cmd.price, cmd.quantity, cmd.user_id);

        case CommandType::CANCEL:
            return engine_.processCancel(cmd.order_id, cmd.user_id);

        case CommandType::BOOK:
            return engine_.getBook(cmd.levels); // 默认 5 档

        default:
            return "ERROR invalid_command";
    }
}