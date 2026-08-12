#pragma once

#include <cstring>
#include <vector>
#include <list>
#include <unordered_map>
#include <atomic>

#include <cstddef>

#include "spsc_byte_ring.h"
#include "io_uring_poller.h"
#include "matching_engine.h"
#include "protocol.h"
#include "metrics.h"
#include "itch_parser.h"

// SPSC ring 状态快照（独立 shm），供监控端 mmap 直接读取
struct RingStatus {
    uint64_t tail;       // IO 线程写
    uint64_t head;       // Send 线程写
    uint64_t capacity;   // RING_SIZE
};

// 连接上下文：读缓冲（仅 IO+Matching 线程使用）
// 累积缓冲（accum_buf_）：io_uring 固定缓冲区每次 recv 从头写，不支持半包跨 recv 累积。
// 每次 recv 后把固定缓冲数据 append 到累积缓冲，解析从累积缓冲读——正确处理变长 ITCH
// 帧跨包拆分（如 2B 长度前缀单独到达）。
struct ConnContext
{
    int fd;
    char* read_buf = nullptr;  // 指向 io_uring 固定缓冲区（内核临时落点）
    uint32_t buf_idx = UINT32_MAX;  // 固定缓冲区索引，用于 re-arm recv
    size_t pending = 0;             // 累积缓冲中有效字节数
    size_t consumed = 0;            // 累积缓冲中已消费偏移
    std::vector<char> accum_buf;    // 累积缓冲（半包跨 recv 保留）

    bool closing = false;                      // 正在关闭，不再提交 recv
    std::atomic<bool> close_acked{false};      // Send 线程 close(fd) 后置 true

    // 把固定缓冲刚 recv 到的 bytes 字节 append 到累积缓冲
    void append_read(int bytes)
    {
        accum_buf.insert(accum_buf.end(), read_buf, read_buf + bytes);
        pending = accum_buf.size();
    }

    // 已消费字节从累积缓冲头部移除
    void compact()
    {
        if (consumed == 0) return;
        size_t remaining = pending - consumed;
        if (remaining > 0)
            memmove(accum_buf.data(), accum_buf.data() + consumed, remaining);
        accum_buf.resize(remaining);
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
        int wake_fd,
        ItchParser& parser,            // 共享 ITCH 解析器（symbol→locate 映射）
        IOCounters* metrics,
        RingStatus* ring_status = nullptr,
        uint64_t* io_heartbeat = nullptr,
        uint64_t* send_heartbeat = nullptr
    );

    ~TcpServer();

    // io_uring 事件循环
    void start();

private:
    // 新连接到达（由 io_uring accept CQE 触发）
    void onAccept(int client_fd);

    // recv 完成：bytes_read 是从 io_uring CQE 获取的读取字节数
    void onRecv(ConnContext* conn, int bytes_read);

    void closeConnection(ConnContext* conn);
    void drainPendingClose();

    // 从累积缓冲解析所有完整 ITCH 帧到 out（半包保留，不阻塞）
    void drainBuffered(ConnContext* conn, std::vector<BinaryResponse>& out);
    void logSummary();
    void pushResponses(int fd, const std::vector<BinaryResponse>& buf);
    void notifySendThread();

    // 解析一条 ITCH 消息并交给撮合引擎
    void processItchMessage(
        const uint8_t* msg, size_t len,
        std::vector<BinaryResponse>& out_responses
    );

private:
    int port_ = 0;
    int server_fd_ = -1;
    MatchingEngine& engine_;
    SPSCByteRing<RING_SIZE>& ring_;
    int wake_fd_ = -1;

    IoUringPoller poller_;
    ItchParser& itch_parser_;                         // 共享 ITCH 解析器（外部持有）
    std::unordered_map<int, ConnContext*> conns_;
    std::list<ConnContext*> pending_closes_;  // 等待 Send 确认的关闭连接（尾插，老的在队首）
    IOCounters* metrics_ = nullptr;     // 指向 shared.io
    RingStatus* ring_status_ = nullptr;
    uint64_t*   io_heartbeat_ = nullptr;
    uint64_t*   send_heartbeat_ = nullptr;
    uint64_t summary_last_orders_ = 0;
};
