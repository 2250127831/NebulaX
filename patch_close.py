import sys

with open("src/tcp_server.cpp", "r") as f:
    content = f.read()

# Replace closeConnection - revert to immediate cleanup
old_close = """void TcpServer::closeConnection(ConnContext* conn)
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
}"""

new_close = """void TcpServer::closeConnection(ConnContext* conn)
{
    int fd = conn->fd;
    conn->closing = true;

    // 推 RSP_CLOSE，Send 线程 close(fd)
    BinaryResponse frame;
    frame.type = RSP_CLOSE;
    frame.data.header.client_fd = fd;
    frame.data.header.count = 0;
    frame.data.header.ack_ptr = nullptr;

    size_t retries = 0;
    while (ring_.push(&frame, sizeof(BinaryResponse)) == 0) {
        notifySendThread();
        if (++retries > 10000) break;
        __builtin_ia32_pause();
    }
    notifySendThread();

    // 立即回收（不等 Send 确认，与原版一致）
    poller_.free_buffer(conn->buf_idx);
    conns_.erase(fd);
    delete conn;
}"""

assert old_close in content, "closeConnection not found!"
content = content.replace(old_close, new_close)

# drainPendingClose becomes a no-op
old_drain = """void TcpServer::drainPendingClose()
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
}"""

new_drain = """void TcpServer::drainPendingClose()
{
    // closeConnection 已立即回收，无需 pending 队列
    (void)this;
}"""

assert old_drain in content, "drainPendingClose not found!"
content = content.replace(old_drain, new_drain)

# Patch graceful shutdown to use immediate cleanup
old_shutdown = """    // 第二遍：未关闭的推 RSP_CLOSE，已在 pending 中的跳过
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
    drainPendingClose();"""

new_shutdown = """    // 第二遍：未关闭的推 RSP_CLOSE，立即回收
    for (auto& [fd, conn] : conns_) {
        if (conn->closing) continue;

        conn->closing = true;

        BinaryResponse frame;
        frame.type = RSP_CLOSE;
        frame.data.header.client_fd = fd;
        frame.data.header.count = 0;
        frame.data.header.ack_ptr = nullptr;

        size_t retries = 0;
        while (ring_.push(&frame, sizeof(BinaryResponse)) == 0) {
            notifySendThread();
            if (++retries > 10000) break;
            __builtin_ia32_pause();
        }
        notifySendThread();

        poller_.free_buffer(conn->buf_idx);
        conns_.erase(fd);
        delete conn;
    }

    // 等 ring 排空
    while (ring_.free_space() < RING_SIZE) {
        notifySendThread();
        __builtin_ia32_pause();
    }"""

assert old_shutdown in content, "shutdown code not found!"
content = content.replace(old_shutdown, new_shutdown)

with open("src/tcp_server.cpp", "w") as f:
    f.write(content)
print("OK")
