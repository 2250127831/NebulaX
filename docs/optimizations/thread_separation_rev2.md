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
    ├─ ring 空? → send（快速路径）
    └─ ring 非空 / EAGAIN 超阈值 → push ring + write(eventfd)
```

**Ring 设计：** 纯字节缓冲区（1MB），push/pop 内部自动处理环形 wrap。数据流用 `RSP_HEADER` 帧（新增 type 0x87）做消息边界——IO 线程推送 `[RSP_HEADER(fd, count) + count 帧响应]`，Send 线程读 header → 取 fd + count → `read_acquire` 拿到 ring 内部指针直接 `send()`，零拷贝。

**快速路径：** ring 为空时 IO 线程直接 send，绕过 ring 和 Send 线程。连续 500 次 EAGAIN（`sent==0`）才切换到 ring 路径。低负载下 Send 线程全程休眠，延迟零退化。

**通知机制：** eventfd blocking read 替代 epoll。IO 线程仅在推送 ring 后写 eventfd（批粒度，非命令粒度）。Send 线程 `read(wake_fd)` 阻塞等唤醒。

### 改动文件

| 文件 | 改动 |
|------|------|
| `include/protocol.h` | 新增 `RSP_HEADER = 0x87` + union `header { client_fd, count }` |
| `include/spsc_byte_ring.h` | 新建，字节级 SPSC ring，push/pop/read_acquire/read_release |
| `include/tcp_server.h` | `ConnContext` 去掉 resp_buf/sent/write_interested；构造器加 ring + wake_fd |
| `src/tcp_server.cpp` | `handleRead` 攒批推送；`closeConnection` 直接关 fd |
| `src/main.cpp` | 双线程框架；eventfd blocking read；`--io-core`/`--send-core` 绑核参数 |

未改动：`matching_engine`、`order_book`、`order.h`、`protocol.cpp`、`benchmark_client`、`test_correctness`。

---

## 对比数据

### 关键指标一览

| 指标 | Phase 4 (epoll) | Phase 5 rev2 | Δ |
|:----|:--------------:|:------------:|:-:|
| **Pipeline QPS** | **13,504,712** | **19,212,647** | **+42%** |
| **Pipeline avg** | 24 ms | 21 ms | — |
| **Ping-pong avg** | 8 µs | **8 µs** | 不变 |
| **Ping-pong P50** | 8 µs | 8 µs | 不变 |
| **Ping-pong P999** | 30 µs | **27 µs** | -10% |
| **sendto/run** | 31,308 | 98,029 | +3.1× |
| **recvfrom/run** | 105,649 | 105,649 | 不变 |
| **IPC (pipeline)** | 1.39 | 1.40 | 不变 |
| **ctx/s** | 54,100¹ | 76,724 | +42%（双线程正常开销） |

### 逐轮明细

#### Pipeline（4 连接）

| 轮次 | avg | P50 | P99 | P999 | QPS |
|-----|----:|----:|----:|----:|----:|
| 1 | 23 ms | 17 ms | 46 ms | 46 ms | 18,105,207 |
| 2 | 20 ms | 14 ms | 43 ms | 43 ms | 19,583,665 |
| 3 | 20 ms | 14 ms | 40 ms | 40 ms | 19,949,069 |
| **平** | **21 ms** | **15 ms** | **43 ms** | **43 ms** | **19,212,647** |

#### Ping-pong

| 轮次 | avg | P50 | P99 | P999 | QPS |
|-----|----:|----:|----:|----:|----:|
| 1 | 8 µs | 8 µs | 11 µs | 26 µs | 118,246 |
| 2 | 8 µs | 8 µs | 11 µs | 27 µs | 121,033 |
| 3 | 9 µs | 9 µs | 12 µs | 28 µs | 114,107 |
| **平** | **8 µs** | **8 µs** | **11 µs** | **27 µs** | **117,796** |

---

## 性能分析

### Pipeline QPS +42% 的来源

Phase 5 rev2 的 IO 线程比 Phase 4 更高效，不是因为双线程并行，而是因为去掉了 per-connection 的响应管理：

- **Phase 4**：`handleRead` 后调 `trySendResponses`，涉及 `conn->resp_buf` 的 `emplace_back`、`memmove`（移除已发送帧）、`epoll_ctl(MOD, EPOLLOUT)` 注册注销、`write_interested` 状态跟踪
- **Phase 5 rev2**：`handleRead` 积累响应到临时 `vector`，ET drain 结束后一次性 `pushResponses`。send 直出，无中间状态

QPS 本身是 IO 单线程打出的（快速路径覆盖了所有数据，Send 线程在 benchmark 期间持续休眠），数据在 500K 到 5000 万命令范围内一致。

### Ping-pong 延迟零退化

快速路径保证了 ring 空时 IO 线程直接 send。ping-pong 模式下每次推送前 ring 都是空的（上一条响应的 send 已经完成），因此全部走快速路径。`syscalls:sys_enter_read = 0` 验证了 Send 线程全程未被唤醒。

P999 从 Phase 4 的 30µs 降到 27µs（-10%），差异在测量噪声内。更重要的是没有退化。

### 双线程何时发挥作用

在 loopback benchmark 下，快速路径总是成功，ring 路径不被触发。双线程的架构价值在以下场景体现：

1. **send 阻塞隔离**：实际网络条件下 send 可能 EAGAIN 持续。单线程会被拖停撮合；双线程下 IO 线程检测到连续 EAGAIN 后 fall through 到 ring，Send 线程接手剩余发送，IO 继续处理新数据
2. **多输出路径预留**：交易确认、行情推送、风控日志可各自对接独立 ring + Send 线程，架构上扩展成本低

### 与前代 Phase 5 对比

当前实现与此前 `phase5-双线程` 分支的关键差异：

| 方面 | Phase 5 (旧) | Phase 5 rev2 |
|:----|:-----------:|:------------:|
| Ring entry | RespEntry（200B，4 cache line）| 48B 等长 slot，可零拷贝 send |
| 跨核传输 | 200B/entry 穿透 L2 | 48B/entry |
| 数据拷贝 | producer memcpy + consumer memcpy | read_acquire 返回 ring 内部指针，send 零拷贝 |
| 通知方式 | eventfd epoll per-response | eventfd blocking read per-batch |
| Send 线程 epoll | eventfd + EPOLLOUT 管理 | 无，用阻塞 eventfd read |
| QPS | 4.73M | **19.2M** |

---

## 火焰图对比

### Pipeline（IO+Matching 线程）

| 分类 | Phase 4 | Phase 5 rev2 | 变化说明 |
|:----|:-------:|:------------:|---------|
| 撮合逻辑 | ~59% | ~61% | 占比相近，仍是瓶颈 |
| send 路径 | ~18.8% | ~22.9% | 快速路径下 IO 线程自发送 |
| recv 路径 | ~8% | ~8% | 不变 |
| epoll_wait | ~0% | ~0% | 多连接消除空转 |
| 管理开销 | ~14% | ~8% | 去掉 resp_buf/epoll_ctl，省下的 |

---

## 硬件事件

| 事件 | Phase 4 | Phase 5 rev2 | 变化 |
|:----|:-------:|:------------:|:----:|
| IPC | 1.39 | 1.40 | 不变 |
| cycles | 1,340M | 1,143M | -15% |
| syscalls:sendto | 31,308 | 98,029 | +3.1×（每次 send 数据更多） |
| syscalls:recvfrom | 105,649 | 105,649 | 不变 |
| syscalls:read | 0 | 0 | Send 线程全程休眠，无需 epoll |
| ctx/s | 54,100¹ | 76,724 | +42%（双线程正常开销） |

---

## 参数选择

| 参数 | 值 | 依据 |
|------|:--:|------|
| RING_SIZE | 1,048,576 (1MB) | 覆盖 ~0.75ms @ 19M QPS 抖动 |
| fast path EAGAIN 阈值 | 500 spins | 确保 loopback 上不误切 ring |
| Send 线程唤醒 | eventfd blocking read | 比 epoll 简化，一个 syscall 完成 |
| 响应批量推送 | per handleRead | 一次跨核传输替代谢命令推送 |

直觉数值已通过实测验证——各参数在 500K~5000 万命令范围内表现一致，未见退化或异常波动。

¹ Phase 4 ctx/s 来自 epoll_reactor.md（perf 系统级统计），rev2 为进程级（`perf stat -p`），口径不完全一致，仅做参考。

---

## 后续方向

| 阶段 | 内容 | 目的 |
|:----|------|------|
| Phase 6 | 内存池 + 平坦数据结构 | 解决撮合逻辑的 cache miss 瓶颈 |
| Phase 7 | io_uring | send/recv syscall 批量化 |
