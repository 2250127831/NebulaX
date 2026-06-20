#!/usr/bin/env python3
"""NebulaX 实时性能计数器读取工具。
用法: python3 scripts/read_metrics.py

SharedMetrics 布局（16 × uint64_t = 128 bytes）:
  [0] io_thread_pid
  [1] send_thread_pid
  [2-10]  IOCounters (9)
  [11-15] SendCounters (5)
"""

import mmap
import os
import struct
import sys

SHM_PATH = "/dev/shm/nebulaX_metrics"

IO_FIELDS = [
    "recv_frames", "new_orders", "cancels", "book_queries",
    "trades", "errors", "order_pool_used", "order_pool_capacity",
    "tick_counter",
]
SEND_FIELDS = [
    "send_batches", "send_bytes", "send_zc_ok", "send_zc_fail",
    "send_tick_counter",
]

def fmt_bytes(n):
    if n < 1024:
        return f"{n} B"
    elif n < 1024 * 1024:
        return f"{n / 1024:.1f} KB"
    else:
        return f"{n / 1024 / 1024:.1f} MB"

def read_metrics():
    if not os.path.exists(SHM_PATH):
        print("NebulaX 未运行（找不到共享内存）")
        return False

    fd = os.open(SHM_PATH, os.O_RDONLY)
    try:
        mm = mmap.mmap(fd, 128, mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fd)

    data = struct.unpack_from("<16Q", mm, 0)
    io_pid, send_tid = data[0], data[1]
    io_data = data[2:11]
    send_data = data[11:16]

    print(f"── IO Thread (pid={io_pid}) ──")
    for i, name in enumerate(IO_FIELDS):
        if name == "order_pool_capacity":
            continue
        if name == "order_pool_used":
            cap = io_data[IO_FIELDS.index("order_pool_capacity")]
            pct = f" ({io_data[i] * 100 / cap:.1f}%)" if cap > 0 else ""
            print(f"  order_pool: {io_data[i]}/{cap}{pct}")
        else:
            print(f"  {name}: {io_data[i]}")

    print(f"\n── Send Thread (tid={send_tid}) ──")
    for i, name in enumerate(SEND_FIELDS):
        val = send_data[i]
        if name == "send_bytes":
            print(f"  send_bytes: {fmt_bytes(val)}")
        else:
            print(f"  {name}: {val}")

    return True

if __name__ == "__main__":
    sys.exit(0 if read_metrics() else 1)
