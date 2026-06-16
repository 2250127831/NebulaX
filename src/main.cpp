#include "matching_engine.h"
#include "protocol.h"
#include "tcp_server.h"

#include <csignal>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <cstdlib>

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include "send_uring.h"
#include "shutdown_guard.h"
#include "metrics.h"

int main(int argc, char* argv[])
{
    int port = 2250;
    int io_core   = -1;
    int send_core = -1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--io-core") == 0 && i + 1 < argc)
            io_core = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--send-core") == 0 && i + 1 < argc)
            send_core = std::atoi(argv[++i]);
        else
            port = std::atoi(argv[i]);
    }

    signal(SIGPIPE, SIG_IGN);
    ShutdownGuard::install();

    // ── 共享内存 —— 暴露性能计数器 ──
    const char* shm_path = "/nebulaX_metrics";
    int shm_fd = shm_open(shm_path, O_CREAT | O_RDWR, 0644);
    if (shm_fd < 0) { write(STDERR_FILENO, "ERROR: shm_open failed\n", 23); return 1; }
    ftruncate(shm_fd, sizeof(SharedMetrics));
    auto* shared = static_cast<SharedMetrics*>(mmap(nullptr, sizeof(SharedMetrics),
        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    close(shm_fd);
    if (shared == MAP_FAILED) { write(STDERR_FILENO, "ERROR: mmap failed\n", 19); return 1; }

    MatchingEngine engine(shared ? &shared->io : nullptr);
    engine.loadSnapshot("/tmp/nebulaX_snapshot.dat");
    SPSCByteRing<RING_SIZE> ring;

    // ── 初始化 io_uring（可选，仅用于 SEND_ZC）──
    struct io_uring send_uring{};
    bool zc_ok = init_send_uring(send_uring, ring);
    std::atomic<bool> io_shutdown_done{false};

    int wake_fd = eventfd(0, 0);
    if (wake_fd < 0) {
        write(STDERR_FILENO, "ERROR: eventfd() failed\n", 24);
        return 1;
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
        TcpServer server(port, engine, ring, wake_fd, shared ? &shared->io : nullptr);
        server.start();
    });

    // ── Send 线程 ──
    auto* send_metrics = shared ? &shared->send : nullptr;

    std::thread send_thread([&, send_metrics, shared]() {
        if (send_core >= 0) {
            cpu_set_t cs;
            CPU_ZERO(&cs);
            CPU_SET(send_core, &cs);
            pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
        }

        if (shared) shared->send_thread_pid = static_cast<uint64_t>(syscall(SYS_gettid));

        while (!io_shutdown_done) {
            uint8_t hdr[48];
            if (ring.pop(hdr, 48) == 0) {
                if (io_shutdown_done) break;
                uint64_t val;
                read(wake_fd, &val, 8);
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

    // 等待 IO 线程退出（start() 内部已排空 ring）
    io_thread.join();
    io_shutdown_done = true;
    // 唤醒 Send 线程，它会看到 io_shutdown_done 后退出
    {
        uint64_t val = 1;
        write(wake_fd, &val, sizeof(val));
    }
    send_thread.join();

    write(STDOUT_FILENO, "saving snapshot...\n", 19);
    engine.saveSnapshot("/tmp/nebulaX_snapshot.dat");
    write(STDOUT_FILENO, "snapshot done\n", 14);

    if (zc_ok) io_uring_queue_exit(&send_uring);
    munmap(shared, sizeof(SharedMetrics));
    shm_unlink(shm_path);
    close(wake_fd);
    return 0;
}
