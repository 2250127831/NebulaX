#pragma once

#include <liburing.h>
#include "spsc_byte_ring.h"

// 初始化 io_uring 实例并注册 ring 固定缓冲区。
// 失败返回 false，zc_ok 保持 false 即可。
bool init_send_uring(struct io_uring& uring, SPSCByteRing<RING_SIZE>& ring);

// 从 SPSC ring 读数据，通过 IORING_OP_SEND_ZC_FIXED 零拷贝发送。
// 成功返回 len，失败返回负 errno（-EOPNOTSUPP / -ENOSYS 应降级到 plain send）。
ssize_t send_zc_all(SPSCByteRing<RING_SIZE>& ring, struct io_uring& uring,
                    int fd, size_t len, int flags);
