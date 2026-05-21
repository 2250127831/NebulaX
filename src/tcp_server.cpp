#include "tcp_server.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>

TcpServer::TcpServer(int port, MatchingEngine& engine)
    : port_(port), engine_(engine)
{
    // ── create server socket (non-blocking for ET) ──
    server_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
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

    // ── create epoll fd ──
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        write(STDERR_FILENO, "ERROR: epoll_create1() failed\n", 30);
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    // register server socket（ET）
    epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev) < 0) {
        write(STDERR_FILENO, "ERROR: epoll_ctl ADD server failed\n", 35);
        close(epoll_fd_);
        epoll_fd_ = -1;
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    const char msg[] = "NebulaX server listening on port ";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
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
    for (auto& [fd, conn] : conns_) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        delete conn;
    }
    conns_.clear();
    if (epoll_fd_ >= 0) close(epoll_fd_);
    if (server_fd_ >= 0) close(server_fd_);
}

void TcpServer::start()
{
    if (server_fd_ < 0 || epoll_fd_ < 0) {
        write(STDERR_FILENO, "ERROR: server not initialized\n", 30);
        return;
    }

    epoll_event events[MAX_EVENTS];

    while (true) {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            write(STDERR_FILENO, "ERROR: epoll_wait() failed\n", 27);
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server_fd_) {
                handleAccept();
            } else {
                auto* conn = static_cast<ConnContext*>(events[i].data.ptr);
                uint32_t ev = events[i].events;

                // read-side: data / orderly close / error
                // handleRead 内部处理 recv==0 / recv<0
                if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP))
                    handleRead(conn);

                // write-side: 只当连接仍存活时才发
                if (ev & EPOLLOUT && conns_.count(conn->fd))
                    handleWrite(conn);
            }
        }
    }
}

void TcpServer::handleAccept()
{
    while (true) {
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept4(server_fd_, (sockaddr*)&addr, &len,
                                SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            write(STDERR_FILENO, "ERROR: accept4() failed\n", 24);
            break;
        }

        auto* conn = new ConnContext{};
        conn->fd = client_fd;
        conns_[client_fd] = conn;

        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = conn;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev);
    }
}

void TcpServer::handleRead(ConnContext* conn)
{
    // ET: 外层循环确保 kernel buffer 排空到 EAGAIN
    // 每一轮：recv → 处理 → compact，如果 buffer 满导致 recv 中途退出
    // 或处理完所有命令后还有空间，继续 recv
    bool more;
    do {
        more = false;

        // recv: 读到 EAGAIN 或 buffer 满
        while (true) {
            size_t free = sizeof(conn->read_buf) - conn->pending;
            if (free == 0) break;  // 满了，先处理腾空间
            ssize_t n = recv(conn->fd, conn->read_buf + conn->pending, free, 0);
            if (n > 0) {
                conn->pending += n;
                more = true;
            } else if (n == 0) {
                closeConnection(conn);
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                closeConnection(conn);
                return;
            }
        }

        // 处理所有完整命令
        while (conn->pending - conn->consumed >= sizeof(BinaryCommand)) {
            BinaryCommand cmd;
            memcpy(&cmd, conn->read_buf + conn->consumed, sizeof(cmd));
            conn->consumed += sizeof(BinaryCommand);

            if (!validateCommand(cmd)) {
                auto& rsp = conn->resp_buf.emplace_back();
                rsp.type = RSP_ERROR;
                rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_COMMAND_TYPE);
            } else {
                processRequest(cmd, conn->resp_buf);
            }
            more = true;
        }

        conn->compact();

        // more==true: 这轮处理过数据，再试一轮确保 kernel buffer 排空
        // more==false: 没新数据也没处理任何命令，退出
    } while (more);

    trySendResponses(conn);
}

void TcpServer::handleWrite(ConnContext* conn)
{
    trySendResponses(conn);
}

void TcpServer::trySendResponses(ConnContext* conn)
{
    size_t total = conn->resp_buf.size() * sizeof(BinaryResponse);

    // 如果全部发完了（resp_sent == total），compact 清理已发 frames
    if (conn->resp_sent > 0 && conn->resp_sent == total) {
        conn->resp_buf.clear();
        conn->resp_sent = 0;
    }

    if (conn->resp_buf.empty()) {
        if (conn->write_interested) {
            epoll_event ev;
            ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
            ev.data.ptr = conn;
            epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn->fd, &ev);
            conn->write_interested = false;
        }
        return;
    }

    const char* data = reinterpret_cast<const char*>(conn->resp_buf.data());

    while (conn->resp_sent < total) {
        ssize_t n = send(conn->fd, data + conn->resp_sent,
                         total - conn->resp_sent, MSG_NOSIGNAL);
        if (n > 0) {
            conn->resp_sent += n;
        } else if (n == -1 && errno == EAGAIN) {
            break;
        } else {
            closeConnection(conn);
            return;
        }
    }

    // 只移除已完整发送的 frame（resp_sent 可能包含不完整 frame）
    size_t frames_sent = conn->resp_sent / sizeof(BinaryResponse);
    if (frames_sent > 0) {
        size_t remaining = conn->resp_buf.size() - frames_sent;
        if (remaining > 0) {
            memmove(conn->resp_buf.data(),
                    conn->resp_buf.data() + frames_sent,
                    remaining * sizeof(BinaryResponse));
        }
        conn->resp_buf.resize(remaining);
        conn->resp_sent -= frames_sent * sizeof(BinaryResponse);
    }

    // 更新 EPOLLOUT 状态
    bool done = (conn->resp_sent == 0 && conn->resp_buf.empty());
    if (done && conn->write_interested) {
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = conn;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn->fd, &ev);
        conn->write_interested = false;
    } else if (!done && !conn->write_interested) {
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = conn;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn->fd, &ev);
        conn->write_interested = true;
    }
}

void TcpServer::closeConnection(ConnContext* conn)
{
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn->fd, nullptr);
    close(conn->fd);
    conns_.erase(conn->fd);
    delete conn;
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
