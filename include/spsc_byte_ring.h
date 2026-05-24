#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <liburing.h>

// ── SPSCByteRing: 单生产者单消费者的字节环形缓冲区 ──
//
// push(data, len): 写入尽可能多的字节或部分字节后返回
// pop(buf, len):   读出尽可能多的字节或部分字节后返回
// read_acquire / read_release: 直接读取 ring 内部数据（零拷贝发送用）
// N 必须是 2 的幂。
//
// io_uring SEND_ZC 支持：
// init_uring() — 注册缓冲区为 io_uring 固定缓冲区
// send_zc_all(fd, len, flags) — IORING_OP_SEND_ZC，内核 DMA 从 ring 读数据发送
template<size_t N>
class SPSCByteRing
{
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");

public:
    SPSCByteRing() : tail_(0), head_(0) {}
    ~SPSCByteRing()
    {
        if (uring_inited_) close_uring();
    }

    SPSCByteRing(const SPSCByteRing&) = delete;
    SPSCByteRing& operator=(const SPSCByteRing&) = delete;

    // ── io_uring 初始化（SEND_ZC）──

    bool init_uring()
    {
        if (io_uring_queue_init(256, &uring_, 0) < 0)
            return false;
        struct iovec iov = { buf_, N };
        if (io_uring_register_buffers(&uring_, &iov, 1) < 0) {
            io_uring_queue_exit(&uring_);
            return false;
        }
        uring_inited_ = true;
        return true;
    }

    void close_uring()
    {
        if (uring_inited_) {
            io_uring_unregister_buffers(&uring_);
            io_uring_queue_exit(&uring_);
            uring_inited_ = false;
        }
    }

    // ── send_zc_all：从 ring 读取 len 字节并零拷贝发送到 fd ──
    //
    // 内部逐段 read_acquire → 提交 SEND_ZC SQE → 等 CQE → read_release。
    // 成功返回 len，失败返回负错误码（-errno）。
    // 调用方应检查 -EOPNOTSUPP 并回退到普通 send。
    ssize_t send_zc_all(int fd, size_t len, int flags)
    {
        if (!uring_inited_) return -ENOSYS;

        size_t remaining = len;
        while (remaining > 0) {
            const void* ptr;
            size_t chunk = read_acquire(ptr, remaining);
            if (chunk == 0) {
                __builtin_ia32_pause();
                continue;
            }

            struct io_uring_sqe* sqe = io_uring_get_sqe(&uring_);
            if (!sqe) return -EAGAIN;
            io_uring_prep_send_zc_fixed(sqe, fd, ptr, chunk, flags, 0, 0);

            // 等 CQE：跳过通知（res=0），直到拿到正结果或错误
            ssize_t sent = 0;
            bool got = false;
            while (!got) {
                int ret = io_uring_submit_and_wait(&uring_, 1);
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    return -errno;
                }

                struct io_uring_cqe* cqe;
                unsigned head;
                io_uring_for_each_cqe(&uring_, head, cqe) {
                    if (cqe->res == 0) {
                        // 通知 CQE（IORING_CQE_F_NOTIF），buffer 已释放，跳过
                    } else if (cqe->res > 0) {
                        sent = cqe->res;
                        got = true;
                    } else {
                        sent = cqe->res;
                        got = true;
                    }
                    io_uring_cqe_seen(&uring_, cqe);
                }
            }

            if (sent > 0) {
                head_.store(head_.load(std::memory_order_relaxed) + sent,
                            std::memory_order_release);
                remaining -= sent;
            } else if (sent == -EPIPE || sent == -ECONNRESET) {
                read_release(remaining);
                return sent;
            } else {
                // 其他错误（-EOPNOTSUPP、-EAGAIN 等）：释放剩余数据并返回
                read_release(remaining);
                return sent;
            }
        }
        return len;
    }

    // ── producer ──

    size_t push(const void* data, size_t len)
    {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t free = N - (tail - head);
        if (free == 0 || len == 0) return 0;

        size_t actual = (len < free) ? len : free;
        size_t mask   = N - 1;
        size_t pos    = tail & mask;

        size_t n1 = N - pos;
        if (n1 > actual) n1 = actual;
        if (n1 > 0) memcpy(buf_ + pos, data, n1);

        size_t n2 = actual - n1;
        if (n2 > 0)
            memcpy(buf_, static_cast<const uint8_t*>(data) + n1, n2);

        tail_.store(tail + actual, std::memory_order_release);
        return actual;
    }

    // ── consumer: 零拷贝读取接口 ──

    // 获取 ring 内部连续数据的指针。
    // 返回实际可用字节数（0 表示空），ptr 指向 ring 内部缓冲区。
    // 调用方读取后必须用 read_release 归还。
    size_t read_acquire(const void*& ptr, size_t request)
    {
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t head = head_.load(std::memory_order_relaxed);
        size_t used = tail - head;
        if (used == 0 || request == 0) { ptr = nullptr; return 0; }

        size_t actual = (request < used) ? request : used;
        size_t mask   = N - 1;
        size_t pos    = head & mask;
        size_t contig = N - pos;
        if (actual > contig) actual = contig;
        ptr = buf_ + pos;
        return actual;
    }

    // 归还已消费的字节。
    void read_release(size_t bytes)
    {
        head_.store(head_.load(std::memory_order_relaxed) + bytes,
                    std::memory_order_release);
    }

    // ── consumer: 带拷贝的完整读取（内部处理 wrap）──

    size_t pop(void* buf, size_t len)
    {
        const void* ptr;
        size_t n1 = read_acquire(ptr, len);
        if (n1 == 0) return 0;
        memcpy(buf, ptr, n1);
        read_release(n1);

        // 如果 len > n1 说明需要读第二段（wrap 后的数据）
        size_t remaining = len - n1;
        if (remaining > 0) {
            size_t n2 = read_acquire(ptr, remaining);
            if (n2 > 0) {
                memcpy(static_cast<uint8_t*>(buf) + n1, ptr, n2);
                read_release(n2);
                return n1 + n2;
            }
        }
        return n1;
    }

    // ── queries ──

    size_t free_space() const
    {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_relaxed);
        return N - (t - h);
    }

    // ring 缓冲区地址（用于 io_uring 注册为固定缓冲区）
    uint8_t* raw_buffer() { return buf_; }
    size_t   raw_size() const { return N; }

private:
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<size_t> head_{0};
    uint8_t buf_[N];

    bool       uring_inited_ = false;
    struct io_uring uring_{};
};
