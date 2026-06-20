#include "send_uring.h"
#include <cerrno>
#include <unistd.h>

bool init_send_uring(struct io_uring& uring, SPSCByteRing<RING_SIZE>& ring)
{
    if (io_uring_queue_init(256, &uring, 0) < 0)
        return false;

    struct iovec iov = ring.raw_iovec();
    if (io_uring_register_buffers(&uring, &iov, 1) < 0) {
        io_uring_queue_exit(&uring);
        return false;
    }
    return true;
}

ssize_t send_zc_all(SPSCByteRing<RING_SIZE>& ring, struct io_uring& uring,
                    int fd, size_t len, int flags)
{
    size_t remaining = len;
    while (remaining > 0) {
        const void* ptr;
        size_t chunk = ring.read_acquire(ptr, remaining);
        if (chunk == 0) { __builtin_ia32_pause(); continue; }

        struct io_uring_sqe* sqe = io_uring_get_sqe(&uring);
        if (!sqe) return -EAGAIN;
        io_uring_prep_send_zc_fixed(sqe, fd, ptr, chunk, flags, 0, 0);

        ssize_t sent = 0;
        bool got = false;
        while (!got) {
            int ret = io_uring_submit_and_wait(&uring, 1);
            if (ret < 0) { if (errno == EINTR) continue; return -errno; }
            struct io_uring_cqe* cqe;
            unsigned head;
            io_uring_for_each_cqe(&uring, head, cqe) {
                if (cqe->res > 0) { sent = cqe->res; got = true; }
                else if (cqe->res < 0) { sent = cqe->res; got = true; }
                io_uring_cqe_seen(&uring, cqe);
            }
        }

        if (sent > 0) {
            ring.read_release(sent);
            remaining -= sent;
        } else if (sent == -EPIPE || sent == -ECONNRESET) {
            ring.read_release(remaining);
            return sent;
        } else {
            ring.read_release(remaining);
            return sent;
        }
    }
    return len;
}
