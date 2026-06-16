#include "tcp_server.h"
#include "shutdown_guard.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>

TcpServer::TcpServer(int port, MatchingEngine& engine,
                     SPSCByteRing<RING_SIZE>& resp_ring,
                     int wake_fd)
    : port_(port), engine_(engine), ring_(resp_ring), wake_fd_(wake_fd)
{
    // ── create server socket (non-blocking) ──
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

    if (!poller_.ok()) {
        write(STDERR_FILENO, "ERROR: io_uring_queue_init() failed\n", 37);
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
        close(fd);
        delete conn;
    }
    conns_.clear();

    // 清理 pending close 残留（正常情况下 start() 已排空）
    for (auto* conn : pending_closes_) {
        conns_.erase(conn->fd);
        close(conn->fd);
        delete conn;
    }

    if (server_fd_ >= 0) close(server_fd_);
}

void TcpServer::start()
{
    if (server_fd_ < 0) {
        write(STDERR_FILENO, "ERROR: server not initialized\n", 30);
        return;
    }

    // 初始提交 accept SQE
    poller_.submit_accept(server_fd_);

    while (!ShutdownGuard::isStopping()) {
        int ret = poller_.submit_and_wait_timeout(500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            write(STDERR_FILENO, "ERROR: io_uring_submit_and_wait() failed\n", 42);
            break;
        }

        poller_.process_cqes(server_fd_,
            // on_accept: 新连接到达
            [this](int client_fd) {
                onAccept(client_fd);
                // 重新提交 accept SQE 以接收下一个连接
                poller_.submit_accept(server_fd_);
            },
            // on_recv: 客户端数据到达或 CQE 错误
            [this](int fd, int bytes_read) {
                auto it = conns_.find(fd);
                if (it == conns_.end()) return;
                auto* conn = it->second;

                if (conn->closing) return;  // 已在关闭中，忽略后续 CQE

                if (bytes_read == -EAGAIN) {
                    // transient: 无数据就绪，重新提交 recv
                    poller_.submit_recv(fd, conn->buf_idx);
                    return;
                }

                if (bytes_read < 0) {
                    if (bytes_read != -ECONNRESET && bytes_read != -EPIPE) {
                        write(STDERR_FILENO, "unexpected recv error\n", 22);
                    }
                }

                onRecv(conn, bytes_read);

                // onRecv 可能关闭了连接；若仍存活，重新提交 recv
                auto it2 = conns_.find(fd);
                if (it2 != conns_.end() && !it2->second->closing)
                    poller_.submit_recv(fd, it2->second->buf_idx);
            }
        );

        drainPendingClose();
    }

    // ── 优雅关闭 ──
    close(server_fd_);
    server_fd_ = -1;

    // 第一遍：刷完所有连接上残余的未处理数据
    for (auto& [fd, conn] : conns_) {
        if (conn->pending > conn->consumed)
            onRecv(conn, static_cast<int>(conn->pending - conn->consumed));
    }

    // 第二遍：未关闭的推 RSP_CLOSE，已在 pending 中的跳过
    for (auto& [fd, conn] : conns_) {
        if (conn->closing) continue;

        conn->closing = true;
        conn->close_acked = false;

        BinaryResponse frame;
        frame.type = RSP_CLOSE;
        frame.data.header.client_fd = fd;
        frame.data.header.count = 0;
        frame.data.header.ack_ptr = &conn->close_acked;

        size_t retries = 0;
        while (ring_.push(&frame, sizeof(BinaryResponse)) == 0) {
            notifySendThread();
            if (++retries > 10000) break;
            __builtin_ia32_pause();
        }
        notifySendThread();

        pending_closes_.push_back(conn);
    }

    // 等 ring 排空（Send 线程处理 RSP_CLOSE → close(fd) → 写 ack）
    while (ring_.free_space() < RING_SIZE) {
        notifySendThread();
        __builtin_ia32_pause();
    }

    // 所有关闭已确认，清理 pending 链表
    drainPendingClose();
}

void TcpServer::onAccept(int client_fd)
{
    uint32_t buf_idx = poller_.alloc_buffer();
    if (buf_idx == UINT32_MAX) {
        close(client_fd);
        return;
    }

    // TCP keepalive：检测死连接（网络中断/对端崩溃），活着的空闲连接不受影响
    int keepalive = 1;
    int keepidle  = 10;   // 10s 无数据 → 开始探测
    int keepintvl = 5;    // 探测间隔 5s
    int keepcnt   = 3;    // 连续 3 次无响应 → 断连
    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    auto* conn = new ConnContext{};
    conn->fd = client_fd;
    conn->buf_idx = buf_idx;
    conn->read_buf = poller_.buffer_ptr(buf_idx);
    conns_[client_fd] = conn;

    poller_.submit_recv(client_fd, buf_idx);
}

void TcpServer::onRecv(ConnContext* conn, int bytes_read)
{
    if (bytes_read <= 0) {
        closeConnection(conn);
        return;
    }

    conn->pending += bytes_read;

    std::vector<BinaryResponse> buf;

    // 解析所有完整命令，与 Phase 6 handleRead 的解析循环相同
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
    }

    conn->compact();

    if (!buf.empty())
        pushResponses(conn->fd, buf);
}

void TcpServer::closeConnection(ConnContext* conn)
{
    int fd = conn->fd;
    conn->closing = true;
    conn->close_acked = false;

    // 推 RSP_CLOSE（带 ack 指针），Send 线程 close(fd) 后写回确认
    BinaryResponse frame;
    frame.type = RSP_CLOSE;
    frame.data.header.client_fd = fd;
    frame.data.header.count = 0;
    frame.data.header.ack_ptr = &conn->close_acked;

    size_t retries = 0;
    while (ring_.push(&frame, sizeof(BinaryResponse)) == 0) {
        notifySendThread();
        if (++retries > 10000) break;
        __builtin_ia32_pause();
    }
    notifySendThread();

    // 移入 pending close 列表，等 Send 确认后再回收
    pending_closes_.push_back(conn);
}

void TcpServer::drainPendingClose()
{
    auto it = pending_closes_.begin();
    while (it != pending_closes_.end()) {
        auto* conn = *it;
        if (conn->close_acked.load(std::memory_order_acquire)) {
            poller_.free_buffer(conn->buf_idx);
            conns_.erase(conn->fd);
            delete conn;
            it = pending_closes_.erase(it);
        } else {
            ++it;
        }
    }
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
    // 若 ring 空间不足则 direct send（不阻塞 IO 线程）
    if (ring_.free_space() >= sizeof(BinaryResponse) + bytes) {
        BinaryResponse header;
        header.type = RSP_HEADER;
        header.data.header.client_fd = fd;
        header.data.header.count = count;
        ring_.push(&header, sizeof(BinaryResponse));
        ring_.push(reinterpret_cast<const uint8_t*>(buf.data()), bytes);
        notifySendThread();
    } else {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(buf.data());
        size_t sent = 0;
        while (sent < bytes) {
            ssize_t r = send(fd, data + sent, bytes - sent, MSG_NOSIGNAL);
            if (r > 0) sent += r;
            else if (r == -1 && errno == EAGAIN) __builtin_ia32_pause();
            else break;
        }
    }
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
