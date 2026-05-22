# NebulaX 性能监测报告 — Phase 5 rev2 线程分离

**日期：** 2026-05-22 | **系统：** Ubuntu 22.04 | **网络：** 127.0.0.1 loopback TCP

## 系统配置

| 组件 | 规格 |
|------|------|
| CPU | 12th Gen Intel Core i9-12900HX（Alder Lake，8 P-core + 16 E-core） |
| 最大频率 | 4900 MHz |
| L1d cache | 48K per P-core |
| L2 cache | 1.3M per core，14M total |
| L3 cache | 30M |
| 内存 | 31 GB |
| 内核 | Ubuntu 22.04.5 LTS，x86_64 |
| 绑核 | IO+Matching core 6（P-core），Send core 7（P-core），Client core 5（P-core） |

---

## 测试方法

与前序保持一致：同一命令序列（50% NEW + 25% CANCEL + 25% BOOK，价格区间 10000~14999 重叠），Pipeline 模式每轮 500K 条（4 连接平分），重复 3 轮。新增 10K 条预热消除冷 cache 影响。

**QPS 计算修正：** 多 worker 场景下 QPS 采用 `total_cmds / max_wall_us`（总命令数 / 最慢 worker 耗时），替代旧的 `Σ(count_i / wall_i)` 求和算法。旧算法在多 worker 场景下因各 worker 起止时间差异导致 QPS 虚高约 2 倍。

---

## 架构变更

Phase 4 单线程 epoll 拆分为 IO+Matching 和 Send 两线程，通过 SPSC 字节环形缓冲区连接。

```
IO+Matching (core 6)              Send (core 7)
─────────────────────              ──────────────────
epoll(EPOLLIN)                     eventfd blocking read (唤醒)
  → recv                             → ring.pop → RSP_HEADER(fd, count)
  → match                            → read_acquire → send(fd, ptr, ...)
  → pushResponses                    → read_release
    ├─ count ≤ 100? → send（快速路径）
    └─ count > 100  → push ring + write(eventfd)
```

**Ring 设计：** 纯字节缓冲区（1MB），push/pop 内部自动处理环形 wrap。数据流用 `RSP_HEADER` 帧（新增 type 0x87）做消息边界——IO 线程推送 `[RSP_HEADER(fd, count) + count 帧响应]`，Send 线程读 header → 取 fd + count → `read_acquire` 拿到 ring 内部指针直接 `send()`，零拷贝。

**自适应快速路径：** 响应帧数 ≤ 100 时 IO 线程直接 send。ping-pong 走快速路径保持低延迟，pipeline 走 ring 路径激活双线程并行。

**通知机制：** eventfd blocking read 替代 epoll。批粒度通知。

### 改动文件

| 文件 | 改动 |
|------|------|
| `include/protocol.h` | 新增 `RSP_HEADER = 0x87` + union `header { client_fd, count }` |
| `include/spsc_byte_ring.h` | 新建，字节级 SPSC ring |
| `include/tcp_server.h` | `ConnContext` 去掉 resp_buf/sent/write_interested |
| `src/tcp_server.cpp` | `handleRead` 攒批推送；自适应快速路径 |
| `src/main.cpp` | 双线程框架；eventfd blocking read |

---

## 对比数据

### 关键指标一览

| 指标 | Phase 4 | Phase 5 rev2 | Δ |
|:----|:-------:|:------------:|:-:|
| **Pipeline QPS（burst）** | **7.4M** | **8.5M** | **+15%** |
| **Pipeline QPS（持续 50M）** | **4.1M** | **7.2M** | **+76%** |
| **Pipeline avg** | 26 ms | 21 ms | -19% |
| **Ping-pong avg** | 8 µs | **8 µs** | 不变 |
| **Ping-pong P50** | 8 µs | 8 µs | 不变 |
| **Ping-pong P999** | 30 µs | **26 µs** | -13% |
| **IPC** | 1.30 | 1.28 | — |
| **sendto/run** | 31,308 | 81,058 | +2.6× |
| **recvfrom/run** | 105,649 | 105,597 | 不变 |

> 注：Phase 4 原文记载 QPS 13.5M，系 QPS 求和算法虚高。实测重跑修正后为 7.4M。两阶段对比使用同一修正算法、同一环境。

### 逐轮明细

#### Pipeline（4 连接）

| 轮次 | Phase 4 QPS | Phase 5 rev2 QPS |
|:----|:----------:|:----------------:|
| 1 | 6,582,238 | 8,467,544 |
| 2 | 7,422,582 | 8,226,120 |
| 3 | 8,206,408 | 8,841,889 |
| **均** | **7,403,743** | **8,511,851** |

#### Ping-pong

| 轮次 | avg | P50 | P99 | P999 | QPS |
|-----|----:|----:|----:|----:|----:|
| 1 | 8 µs | 8 µs | 12 µs | 25 µs | 119,732 |
| 2 | 9 µs | 8 µs | 12 µs | 26 µs | 117,212 |
| 3 | 8 µs | 8 µs | 11 µs | 26 µs | 120,236 |
| **平** | **8 µs** | **8 µs** | **12 µs** | **26 µs** | **119,060** |

---

## 性能分析

### Pipeline +76%（持续负载）

持续 50M 命令下 Phase 4 QPS 跌至 4.1M（IPC 0.64），Phase 5 rev2 稳定在 7.2M（IPC 0.99）。差距 76%。

原因是 Phase 4 的 `resp_buf` 管理（`emplace_back` / `memmove` / `epoll_ctl`）在长跑中导致内存布局碎片化，cache miss 急剧上升。Phase 5 rev2 去掉了这些 per-connection 状态，路径更短、更可预测。

Burst 500K 下两者差距较小（+15%），因为短时间内存热度高，管理开销未充分暴露。

### 提升来源

Phase 5 rev2 去掉了以下开销：

- **Phase 4**：`handleRead` 后调 `trySendResponses`，涉及 `conn->resp_buf` 的 `emplace_back`、`memmove`（移除已发送帧）、`epoll_ctl(MOD, EPOLLOUT)` 注册注销
- **Phase 5 rev2**：积累响应到临时 `vector`，ET drain 结束后一次性 `pushResponses`。100 帧阈值让 pipeline 的大批量响应走 ring 路径，Send 线程参与发送

### Ping-pong 延迟不变

快速路径保证小批量（1-3 帧）直接 send 不走 ring。`syscalls:sys_enter_read = 30` 确认 ring 路径仅在 pipeline 大 batch 时触发。

---

## 火焰图对比

| 分类 | Phase 4 | Phase 5 rev2 | 说明 |
|:----|:-------:|:------------:|------|
| 撮合逻辑 | ~59% | ~60% | 占比相近，仍是瓶颈 |
| send 路径 | ~18.8% | ~17.5% | Send 线程分担后 IO 侧下降 |
| recv 路径 | ~8% | ~8% | 不变 |
| 管理开销 | ~14% | ~8% | 去掉 resp_buf/epoll_ctl |

---

## 硬件事件

| 事件 | Phase 4 | Phase 5 rev2 | 变化 |
|:----|:-------:|:------------:|:----:|
| IPC | 1.30 | 1.28 | -2% |
| cache-misses | 9,390,601 | 6,575,277 | -30% |
| L1-dcache-load-misses | 17,636,359 | 18,405,322 | +4% |
| L2-load-misses | 1,807,453 | 1,614,898 | -11% |
| syscalls:sendto | 31,308 | 81,058 | +2.6× |
| syscalls:recvfrom | 105,649 | 105,597 | 不变 |
| syscalls:read | 0 | 30 | ring 路径触发 |

---

## 参数选择

| 参数 | 值 | 依据 |
|------|:--:|------|
| RING_SIZE | 1,048,576 | 1MB，覆盖微秒级抖动 |
| fast path 阈值 | 100 帧 | ping-pong 直达，pipeline 切 ring |
| Send 线程唤醒 | eventfd blocking read | 比 epoll 简化 |

---

## 后续方向

| 阶段 | 内容 | 目的 |
|:----|------|------|
| Phase 6 | 内存池 + 平坦数据结构 | 解决撮合逻辑 cache miss 瓶颈 |
| Phase 7 | io_uring | send/recv syscall 批量化 |
