#include "matching_engine.h"
#include "protocol.h"
#include "tcp_server.h"

#include <csignal>
#include <thread>
#include <cstring>
#include <cerrno>
#include <cstdlib>

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>

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

    MatchingEngine engine;
    SPSCByteRing<RING_SIZE> ring;

    // 尝试初始化 io_uring SEND_ZC（失败不致命，后续降级到 plain send）
    bool zc_ok = ring.init_uring();

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
        TcpServer server(port, engine, ring, wake_fd);
        server.start();
    });

    // ── Send 线程 ──
    std::thread send_thread([&]() {
        if (send_core >= 0) {
            cpu_set_t cs;
            CPU_ZERO(&cs);
            CPU_SET(send_core, &cs);
            pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
        }

        while (true) {
            uint8_t hdr[48];
            if (ring.pop(hdr, 48) == 0) {
                uint64_t val;
                read(wake_fd, &val, 8);
                continue;
            }

            auto* rsp = reinterpret_cast<BinaryResponse*>(hdr);
            if (rsp->type == RSP_CLOSE) {
                close(rsp->data.header.client_fd);
                continue;
            }
            if (rsp->type != RSP_HEADER) continue;

            int fd = rsp->data.header.client_fd;
            uint32_t count = rsp->data.header.count;

            size_t need = count * sizeof(BinaryResponse);

            if (zc_ok && need >= 4096) {
                // 大块数据走 SEND_ZC
                ssize_t r = ring.send_zc_all(fd, need, MSG_NOSIGNAL);
                if (r == -EOPNOTSUPP || r == -ENOSYS)
                    zc_ok = false;           // 内核不支持，降级到 plain send
                else if (r < 0)
                    continue;                // 连接断开，取下一条
                else
                    continue;                // 发送完成
            }

            // plain send 路径
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
        }
    });

    io_thread.join();
    send_thread.join();
    return 0;
}
