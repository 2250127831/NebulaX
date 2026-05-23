#include "tcp_server.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>

TcpServer::TcpServer(int port, MatchingEngine& engine,
                     SPSCByteRing<RING_SIZE>& resp_ring,
                     int wake_fd)
    : port_(port), engine_(engine), ring_(resp_ring), wake_fd_(wake_fd)
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
                if (conns_.count(conn->fd) == 0) continue;

                if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP))
                    handleRead(conn);
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
    // ET: do-while 确保 kernel buffer 排空到 EAGAIN
    // buf 在循环外积累，一批命令的响应一次性推送，减少跨核传输次数
    bool more;
    std::vector<BinaryResponse> buf;
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
                if (!buf.empty()) pushResponses(conn->fd, buf);
                closeConnection(conn);
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (!buf.empty()) pushResponses(conn->fd, buf);
                closeConnection(conn);
                return;
            }
        }

        // 处理所有完整命令，响应追加到 buf
        while (conn->pending - conn->consumed >= sizeof(BinaryCommand)) {
            BinaryCommand cmd;
            memcpy(&cmd, conn->read_buf + conn->consumed, sizeof(cmd));
            conn->consumed += sizeof(BinaryCommand);

            if (!validateCommand(cmd)) {
                auto& rsp = buf.emplace_back();
                rsp.type = RSP_ERROR;
                rsp.data.error.code = static_cast<uint16_t>(ErrorCode::INVALID_COMMAND_TYPE);
            } else {
                processRequest(cmd, buf);
            }
            more = true;
        }

        conn->compact();
    } while (more);

    // 一次性推送所有积累的响应
    if (!buf.empty())
        pushResponses(conn->fd, buf);
}

void TcpServer::closeConnection(ConnContext* conn)
{
    int fd = conn->fd;

    // 先通知 Send 线程关闭 fd（确保所有排队的响应已发完再关）
    BinaryResponse frame;
    frame.type = RSP_CLOSE;
    frame.data.header.client_fd = fd;
    frame.data.header.count = 0;
    while (ring_.push(&frame, sizeof(BinaryResponse)) == 0) {
        notifySendThread();
        __builtin_ia32_pause();
    }
    notifySendThread();

    // 再清理 IO 线程资源
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    conns_.erase(fd);
    delete conn;
}

void TcpServer::pushResponses(int fd, const std::vector<BinaryResponse>& buf)
{
    size_t count = buf.size();
    if (count == 0) return;

    size_t bytes = count * sizeof(BinaryResponse);

    // 快速路径：少量响应帧时直接 send，绕过 ring（保持 ping-pong 低延迟）
    if (count <= 100 && ring_.free_space() == RING_SIZE) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(buf.data());
        size_t sent = 0;
        int spins = 0;
        while (sent < bytes) {
            ssize_t r = send(fd, data + sent, bytes - sent, MSG_NOSIGNAL);
            if (r > 0) {
                sent += r;
                spins = 0;
            } else if (r == -1 && errno == EAGAIN) {
                if (sent == 0 && ++spins >= 500) break;
                __builtin_ia32_pause();
            } else {
                return;
            }
        }
        if (sent == bytes) return;
        if (sent > 0) return;
    }

    // 正常路径：推 RSP_HEADER + 响应帧到 ring，Send 线程消费
    BinaryResponse header;
    header.type = RSP_HEADER;
    header.data.header.client_fd = fd;
    header.data.header.count = count;

    while (ring_.push(&header, sizeof(BinaryResponse)) == 0) {
        notifySendThread();
        __builtin_ia32_pause();
    }

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(buf.data());
    size_t remaining = bytes;
    size_t off = 0;
    while (remaining > 0) {
        size_t n = ring_.push(ptr + off, remaining);
        if (n == 0) {
            notifySendThread();
            __builtin_ia32_pause();
            continue;
        }
        off += n;
        remaining -= n;
    }

    notifySendThread();
}

void TcpServer::notifySendThread()
{
    uint64_t val = 1;
    write(wake_fd_, &val, sizeof(val));
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
