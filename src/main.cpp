#include "matching_engine.h"
#include "protocol.h"
#include "tcp_server.h"
#include "tcp_trade_server.h"
#include "wal.h"
#include "trade_pool.h"
#include "crash_guard.h"

#include <csignal>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <memory>

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include "send_uring.h"
#include "shutdown_guard.h"
#include "metrics.h"
#include "logger.h"

// ── 共享内存布局（除 metrics 外的第二块共享内存）──
static constexpr size_t ORDERS_SIZE = (4 << 20) * sizeof(Order);          // 4M × 64B
static constexpr size_t TRADES_SIZE = sizeof(TradePool);                   // 84MB
static constexpr size_t META_SIZE   = 64;                                  // 心跳 + wal_seq
static constexpr size_t BOOK_SIZE   = ORDERS_SIZE + TRADES_SIZE + META_SIZE;

struct BookMeta {
    uint64_t io_heartbeat;
    uint64_t send_heartbeat;
    uint64_t last_wal_seq;
};

int main(int argc, char* argv[])
{
    int port = 2250;
    int trade_port = -1;   // <0 = 不启动交易接入
    int io_core   = -1;
    int send_core = -1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--io-core") == 0 && i + 1 < argc)
            io_core = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--send-core") == 0 && i + 1 < argc)
            send_core = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--trade-port") == 0 && i + 1 < argc)
            trade_port = std::atoi(argv[++i]);
        else
            port = std::atoi(argv[i]);
    }

    signal(SIGPIPE, SIG_IGN);
    ShutdownGuard::install();
    Logger::instance().init("../logs", LOG_INFO);

    // ── 共享内存 —— 订单簿数据 ──
    bool book_recovered = false;  // 从崩溃中恢复？
    int book_fd = shm_open("/nebulaX_book", O_CREAT | O_RDWR, 0644);
    if (book_fd < 0) { LOG_ERROR("book shm_open failed"); return 1; }

    off_t book_size = lseek(book_fd, 0, SEEK_END);
    bool fresh_start = (book_size == 0);
    ftruncate(book_fd, BOOK_SIZE);

    // mmap 集群：OrderPool 起始 + TradePool + Meta
    uint8_t* book_base = (uint8_t*)mmap(nullptr, BOOK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, book_fd, 0);
    close(book_fd);
    if (book_base == MAP_FAILED) { LOG_ERROR("book mmap failed"); return 1; }

    Order*    order_storage = (Order*)book_base;
    TradePool* trade_pool   = (TradePool*)(book_base + ORDERS_SIZE);
    BookMeta*  meta          = (BookMeta*)(book_base + ORDERS_SIZE + TRADES_SIZE);

    // 首次启动时初始化 OrderPool 空闲链表
    OrderPool order_pool(order_storage, 4 << 20, fresh_start);

    if (!fresh_start) {
        LOG_INFO("recovered shared memory: orders=%lu", order_pool.size());
        book_recovered = true;
    }

    // ── 共享内存 —— 性能计数器 ──
    const char* shm_path = "/nebulaX_metrics";
    int shm_fd = shm_open(shm_path, O_CREAT | O_RDWR, 0644);
    if (shm_fd < 0) { LOG_ERROR("shm_open failed"); return 1; }
    ftruncate(shm_fd, sizeof(SharedMetrics));
    auto* shared = static_cast<SharedMetrics*>(mmap(nullptr, sizeof(SharedMetrics),
        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    close(shm_fd);
    if (shared == MAP_FAILED) { LOG_ERROR("mmap failed"); return 1; }

    // ── SPSC ring 状态共享内存（24 bytes）──
    int ring_fd = shm_open("/nebulaX_ring", O_CREAT | O_RDWR, 0644);
    if (ring_fd < 0) { LOG_ERROR("ring shm_open failed"); return 1; }
    ftruncate(ring_fd, sizeof(RingStatus));
    auto* ring_status = static_cast<RingStatus*>(mmap(nullptr, sizeof(RingStatus),
        PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd, 0));
    close(ring_fd);
    if (ring_status == MAP_FAILED) { LOG_ERROR("ring mmap failed"); return 1; }
    ring_status->capacity = RING_SIZE;

    // ── WAL ──
    WalWriter wal;
    if (!wal.init()) {
        LOG_ERROR("WAL init failed");
    }

    // ── MatchingEngine（共享内存中的 OrderPool）──
    // 需要传入 OrderPool* 重构 OrderBook。当前 OrderBook 内部有自己的 pool_，
    // 先跳过重建（保留外部 order_pool 指针供后续使用）
    MatchingEngine engine(&order_pool, shared ? &shared->io : nullptr);
    engine.wal_ = &wal;
    engine.trade_pool_ = trade_pool;
    engine.book_base_ = book_base;
    engine.book_size_ = BOOK_SIZE;

    // 恢复
    if (book_recovered) {
        engine.recoverFromShared(order_storage, 4 << 20);
    } else {
        engine.recoverFromWal("/tmp/nebulaX_wal.dat");
    }

    SPSCByteRing<RING_SIZE>& ring = *new SPSCByteRing<RING_SIZE>();  // 堆分配防栈溢出

    // ── 初始化 io_uring（可选，仅用于 SEND_ZC）──
    struct io_uring send_uring{};
    bool zc_ok = init_send_uring(send_uring, ring);
    std::atomic<bool> io_shutdown_done{false};

    int wake_fd = eventfd(0, 0);
    if (wake_fd < 0) { LOG_ERROR("eventfd() failed"); return 1; }

    // ── 崩溃 handler（传入 WAL fd，崩溃时刷盘）──
    CrashGuard::install(wal.fd());

    // ── 共享 ITCH 解析器（symbol→locate 映射，TcpServer + TcpTradeServer 共用）──
    ItchParser itch_parser;

    // ── 交易接入（TCP 全双工：Trader 下单 → 撮合 → 回 OUCH 回报）──
    // 独立线程，engine 调用与 TcpServer（ITCH 输入）通过 engine_mtx 互斥。
    std::mutex engine_mtx;
    std::unique_ptr<TcpTradeServer> trade_server;
    if (trade_port > 0) {
        trade_server = std::make_unique<TcpTradeServer>(
            static_cast<uint16_t>(trade_port), engine, engine_mtx);
        trade_server->start();
        LOG_INFO("trade server listening on port %d", trade_port);
    }

    // ── IO+Matching 线程 ──
    std::thread io_thread([&]() {
        if (io_core >= 0) {
            cpu_set_t cs;
            CPU_ZERO(&cs);
            CPU_SET(io_core, &cs);
            pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
        }
        if (shared) shared->io_thread_pid = static_cast<uint64_t>(syscall(SYS_gettid));
        TcpServer server(port, engine, ring, wake_fd, itch_parser,
                         shared ? &shared->io : nullptr, ring_status,
                         &meta->io_heartbeat, &meta->send_heartbeat);
        server.start();
    });

    // ── Send 线程 ──
    auto* send_metrics = shared ? &shared->send : nullptr;

    std::thread send_thread([&, send_metrics, shared, ring_status]() {
        if (send_core >= 0) {
            cpu_set_t cs;
            CPU_ZERO(&cs);
            CPU_SET(send_core, &cs);
            pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
        }

        if (shared) shared->send_thread_pid = static_cast<uint64_t>(syscall(SYS_gettid));

        while (!io_shutdown_done) {
            if (meta) meta->send_heartbeat++;
            if (send_metrics) send_metrics->send_tick_counter++;
            if (ring_status) ring_status->head = ring.head();

            uint8_t hdr[48];
            if (ring.pop(hdr, 48) == 0) {
                if (io_shutdown_done) break;
                // poll 1s 超时，既等数据又维持心跳
                struct pollfd pfd = {wake_fd, POLLIN, 0};
                if (poll(&pfd, 1, 1000) > 0) {
                    uint64_t ev;
                    read(wake_fd, &ev, 8);
                }
                continue;
            }

            auto* rsp = reinterpret_cast<BinaryResponse*>(hdr);
            if (rsp->type == RSP_CLOSE) {
                close(rsp->data.header.client_fd);
                rsp->data.header.ack_ptr->store(true, std::memory_order_release);
                continue;
            }
            if (rsp->type != RSP_HEADER) continue;

            int fd = rsp->data.header.client_fd;
            uint32_t count = rsp->data.header.count;
            size_t need = count * sizeof(BinaryResponse);

            if (send_metrics) send_metrics->send_batches++;

            // ── SEND_ZC 路径 ──
            if (zc_ok && need >= 4096) {
                ssize_t r = send_zc_all(ring, send_uring, fd, need, MSG_NOSIGNAL);
                if (r == -EOPNOTSUPP || r == -ENOSYS)
                    zc_ok = false;
                else if (r < 0) {
                    if (send_metrics) send_metrics->send_zc_fail++;
                    continue;
                } else {
                    if (send_metrics) { send_metrics->send_zc_ok++; send_metrics->send_bytes += need; }
                    continue;
                }
            }

            // ── plain send 路径 ──
            size_t sent = 0;
            while (sent < need) {
                const void* ptr;
                size_t chunk = ring.read_acquire(ptr, need - sent);
                if (chunk == 0) { __builtin_ia32_pause(); continue; }

                ssize_t r = send(fd, ptr, chunk, MSG_NOSIGNAL);
                if (r > 0) {
                    ring.read_release(r);
                    sent += r;
                } else if (r == -1 && errno == EAGAIN) {
                    __builtin_ia32_pause();
                } else {
                    ring.read_release(chunk);
                    break;
                }
            }
            if (send_metrics) send_metrics->send_bytes += sent;
        }

    });

    // 等待 IO 线程退出
    io_thread.join();
    io_shutdown_done = true;
    {
        uint64_t val = 1;
        write(wake_fd, &val, sizeof(val));
    }
    send_thread.join();

    // 停止交易接入（若启用）
    if (trade_server) trade_server->stop();

    LOG_INFO("saving snapshot...");
    engine.saveSnapshot("/tmp/nebulaX_snapshot.dat");
    LOG_INFO("snapshot done");

    // ── 清理 ──
    wal.close();
    if (zc_ok) io_uring_queue_exit(&send_uring);
    munmap(shared, sizeof(SharedMetrics));
    shm_unlink(shm_path);
    munmap(ring_status, sizeof(RingStatus));
    shm_unlink("/nebulaX_ring");
    munmap(book_base, BOOK_SIZE);
    shm_unlink("/nebulaX_book");
    close(wake_fd);
    Logger::instance().shutdown();
    return 0;
}
