#pragma once

#include <liburing.h>
#include <sys/socket.h>
#include <ctime>
#include <netinet/in.h>
#include <functional>
#include <cstdint>

// io_uring 事件轮询器，替代 epoll_wait + recv 循环。
// 只负责 recv 路径——send 路径（SPSC ring + eventfd）完全不碰。
class IoUringPoller
{
public:
    static constexpr unsigned DEFAULT_ENTRIES = 256;
    static constexpr unsigned MAX_BUFFERS = 64;
    static constexpr size_t   BUF_SIZE   = 4096;

    IoUringPoller(unsigned entries = DEFAULT_ENTRIES)
    {
        if (io_uring_queue_init(entries, &ring_, 0) < 0)
            return;

        // 固定缓冲区池：预注册，recv 免去每次 get_user_pages
        for (unsigned i = 0; i < MAX_BUFFERS; ++i) {
            iovs_[i].iov_base = bufs_[i];
            iovs_[i].iov_len  = BUF_SIZE;
            buf_free_stack_[i] = MAX_BUFFERS - 1 - i;  // 倒序入栈
        }
        buf_free_top_ = MAX_BUFFERS;

        if (io_uring_register_buffers(&ring_, iovs_, MAX_BUFFERS) < 0) {
            io_uring_queue_exit(&ring_);
            return;
        }
        ok_ = true;
    }

    ~IoUringPoller()
    {
        if (ok_) {
            io_uring_unregister_buffers(&ring_);
            io_uring_queue_exit(&ring_);
        }
    }

    IoUringPoller(const IoUringPoller&) = delete;
    IoUringPoller& operator=(const IoUringPoller&) = delete;

    bool ok() const { return ok_; }

    // ── 固定缓冲区管理 ──

    uint32_t alloc_buffer()
    {
        if (buf_free_top_ == 0) return UINT32_MAX;
        return buf_free_stack_[--buf_free_top_];
    }

    void free_buffer(uint32_t idx)
    {
        if (buf_free_top_ < MAX_BUFFERS)
            buf_free_stack_[buf_free_top_++] = idx;
    }

    char* buffer_ptr(uint32_t idx) { return bufs_[idx]; }

    // ── SQE 提交 ──

    // 使用固定缓冲区的 recv
    bool submit_recv(int fd, uint32_t buf_idx)
    {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) return false;
        io_uring_prep_recv(sqe, fd, bufs_[buf_idx], BUF_SIZE, 0);
        sqe->buf_index = buf_idx;
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(fd)));
        return true;
    }

    bool submit_accept(int server_fd)
    {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) return false;
        accept_addrlen_ = sizeof(accept_addr_);
        io_uring_prep_accept(sqe, server_fd,
                             reinterpret_cast<struct sockaddr*>(&accept_addr_),
                             &accept_addrlen_,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(server_fd)));
        return true;
    }

    int submit_and_wait()
    {
        return io_uring_submit_and_wait(&ring_, 1);
    }

    // 带超时的 submit_and_wait（用于优雅关闭时不被永久阻塞）
    // timeout_ms 后即使无 CQE 也会返回
    int submit_and_wait_timeout(uint64_t timeout_ms)
    {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) return submit_and_wait();
        struct timespec ts;
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        io_uring_prep_timeout(sqe, (struct __kernel_timespec*)&ts, 1, 0);
        return io_uring_submit_and_wait(&ring_, 1);
    }

    void process_cqes(int server_fd,
                      const std::function<void(int)>& on_accept,
                      const std::function<void(int, int)>& on_recv)
    {
        struct io_uring_cqe* cqe;
        unsigned head;

        io_uring_for_each_cqe(&ring_, head, cqe) {
            int fd = static_cast<int>(reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));
            int res = cqe->res;

            if (fd == server_fd) {
                if (res >= 0)
                    on_accept(res);
            } else {
                on_recv(fd, res);
            }

            io_uring_cqe_seen(&ring_, cqe);
        }
    }

private:
    struct io_uring ring_{};
    bool ok_ = false;
    struct sockaddr_in accept_addr_{};
    socklen_t accept_addrlen_ = 0;

    // 固定缓冲区池
    char bufs_[MAX_BUFFERS][BUF_SIZE]{};
    struct iovec iovs_[MAX_BUFFERS]{};
    uint32_t buf_free_stack_[MAX_BUFFERS]{};
    uint32_t buf_free_top_ = 0;
};
