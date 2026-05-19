#include "tcp_server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

TcpServer::TcpServer(int port, MatchingEngine& engine)
    : port_(port), engine_(engine)
{
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        write(STDERR_FILENO, "ERROR: socket() failed\n", 23);
        return;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        write(STDERR_FILENO, "ERROR: bind() failed\n", 21);
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    if (listen(server_fd_, 10) < 0) {
        write(STDERR_FILENO, "ERROR: listen() failed\n", 23);
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    const char msg[] = "NebulaX server listening on port ";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    // 端口号数字转字符输出（简单实现，不含 locale）
    char pbuf[16];
    int len = 0;
    int tmp = port_;
    do { pbuf[len++] = '0' + tmp % 10; tmp /= 10; } while (tmp);
    for (int i = 0; i < len / 2; ++i) {
        char c = pbuf[i];
        pbuf[i] = pbuf[len - 1 - i];
        pbuf[len - 1 - i] = c;
    }
    write(STDOUT_FILENO, pbuf, len);
    write(STDOUT_FILENO, "\n", 1);
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
        write(STDERR_FILENO, "ERROR: server not initialized\n", 30);
        return;
    }

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            write(STDERR_FILENO, "ERROR: accept() failed\n", 23);
            continue;
        }

        handleClient(client_fd);
        close(client_fd);
    }
}

void TcpServer::handleClient(int client_fd)
{
    char buf[4096];
    size_t pending = 0;
    std::vector<BinaryResponse> responses;
    responses.reserve(8);

    ssize_t n;
    while ((n = recv(client_fd, buf + pending, sizeof(buf) - pending, 0)) > 0) {
        pending += n;

        size_t consumed = 0;
        while (pending - consumed >= sizeof(BinaryCommand)) {
            BinaryCommand cmd;
            memcpy(&cmd, buf + consumed, sizeof(cmd));
            consumed += sizeof(BinaryCommand);

            responses.clear();
            if (!validateCommand(cmd)) {
                auto& rsp = responses.emplace_back();
                rsp.type = RSP_ERROR;
                rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_COMMAND_TYPE);
            } else {
                processRequest(cmd, responses);
            }

            if (!responses.empty()) {
                send(client_fd, responses.data(),
                     responses.size() * sizeof(BinaryResponse), 0);
            }
        }

        size_t remaining = pending - consumed;
        if (remaining > 0 && remaining < pending) {
            memmove(buf, buf + consumed, remaining);
        }
        pending = remaining;
    }
    // 客户端断开或出错
}

void TcpServer::processRequest(const BinaryCommand& cmd, std::vector<BinaryResponse>& out)
{
    switch (cmd.type) {
        case CMD_NEW: {
            Side side = (cmd.side == SIDE_BUY) ? Side::BUY : Side::SELL;
            engine_.processNewOrder(side, cmd.price, cmd.quantity, cmd.user_id, out);
            break;
        }
        case CMD_CANCEL:
            engine_.processCancel(cmd.order_id, cmd.user_id, out);
            break;
        case CMD_BOOK: {
            auto& rsp = out.emplace_back();
            engine_.getBook(rsp);
            break;
        }
    }
}
