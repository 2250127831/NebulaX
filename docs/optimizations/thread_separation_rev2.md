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

扩展测试：2 客户端实例各 2500 万条（8 连接），绑核 5 和 3，去除 perf 干扰测量双线程极限。

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

**自适应快速路径：** 响应帧数 ≤ 100 时 IO 线程直接 send，绕过 ring。ping-pong 每次推送 1-3 帧走快速路径，延迟零退化。pipeline 积累大批响应（128+ 帧）自动切 ring 路径，激活双线程并行。

**通知机制：** eventfd blocking read 替代 epoll。IO 线程仅在推送 ring 后写 eventfd（批粒度）。Send 线程 `read(wake_fd)` 阻塞等唤醒。

### 改动文件

| 文件 | 改动 |
|------|------|
| `include/protocol.h` | 新增 `RSP_HEADER = 0x87` + union `header { client_fd, count }` |
| `include/spsc_byte_ring.h` | 新建，字节级 SPSC ring，push/pop/read_acquire/read_release |
| `include/tcp_server.h` | `ConnContext` 去掉 resp_buf/sent/write_interested；构造器加 ring + wake_fd |
| `src/tcp_server.cpp` | `handleRead` 攒批推送；自适应快速路径（100 帧阈值）|
| `src/main.cpp` | 双线程框架；eventfd blocking read；`--io-core`/`--send-core` 绑核参数 |

未改动：`matching_engine`、`order_book`、`order.h`、`protocol.cpp`、`benchmark_client`、`test_correctness`。

---

## 对比数据

### 关键指标一览

| 指标 | Phase 4 (epoll) | Phase 5 rev2 | Δ |
|:----|:--------------:|:------------:|:-:|
| **Pipeline QPS** | **13,504,712** | **19,166,318** | **+42%** |
| **Pipeline QPS（扩展测试）** | — | **33,865,432** | **双线程满负荷** |
| **Pipeline avg** | 24 ms | 21 ms | — |
| **Ping-pong avg** | 8 µs | **8 µs** | 不变 |
| **Ping-pong P50** | 8 µs | 8 µs | 不变 |
| **Ping-pong P999** | 30 µs | **26 µs** | -13% |
| **sendto/run** | 31,308 | 81,058 | +2.6× |
| **recvfrom/run** | 105,649 | 105,597 | 不变 |
| **IPC (pipeline)** | 1.39 | 1.28 | -8%（双线程跨核开销）|

### 逐轮明细

#### Pipeline（4 连接，标准）

| 轮次 | avg | P50 | P99 | P999 | QPS |
|-----|----:|----:|----:|----:|----:|
| 1 | 23 ms | 17 ms | 47 ms | 47 ms | 18,088,287 |
| 2 | 20 ms | 15 ms | 40 ms | 40 ms | 19,874,879 |
| 3 | 21 ms | 15 ms | 42 ms | 42 ms | 19,535,787 |
| **平** | **21 ms** | **15 ms** | **43 ms** | **43 ms** | **19,166,318** |

#### Pipeline（扩展：8 连接，无 perf）

| 轮次 | avg | P50 | P99 | P999 | QPS |
|-----|----:|----:|----:|----:|----:|
| Client 1 avg | 22.5 ms | 16.3 ms | 42.4 ms | 42.5 ms | 17,712,347 |
| Client 2 avg | 27.4 ms | 19.0 ms | 56.8 ms | 56.9 ms | 16,153,085 |
| **合计** | — | — | — | — | **33,865,432** |

#### Ping-pong

| 轮次 | avg | P50 | P99 | P999 | QPS |
|-----|----:|----:|----:|----:|----:|
| 1 | 8 µs | 8 µs | 12 µs | 25 µs | 119,732 |
| 2 | 9 µs | 8 µs | 12 µs | 26 µs | 117,212 |
| 3 | 8 µs | 8 µs | 11 µs | 26 µs | 120,236 |
| **平** | **8 µs** | **8 µs** | **12 µs** | **26 µs** | **119,060** |

---

## 性能分析

### Pipeline +42% 的构成

标准测试 19.2M QPS 的 +42% 来自两部分：
- **IO 线程效率提升**：去掉 Phase 4 的 `resp_buf` 管理、`epoll_ctl` 注册注销、`write_interested` 状态跟踪。这部分提升与单线程还是双线程无关
- **双线程并行**：100 帧阈值让 pipeline 的大批量响应（128+ 帧）走 ring 路径，Send 线程参与发送。`syscalls:sys_enter_read = 30` 确认 ring 路径被触发

### 扩展测试：35.8M QPS 双线程满负荷

增加负载至 8 连接 5000 万命令后，双线程架构充分激活：

| 模式 | QPS | 两个核的分工 |
|:----|:---:|:------------|
| 单线程纯 send（对比） | 22.7M | match + send 都在 IO 核 |
| 双线程 ring-only（强制 ring） | 35.8M | IO 匹配推 ring，Send pop 发送，并行 |
| 双线程自适应（阈值 100） | **33.9M** | 大 batch 切 ring，小 batch 直达 |

ring 让 IO 线程无需等待 send 完成，两核在时间上重叠。35.8M 是架构的上限估算，自适应版本的 33.9M 是生产可用值（保留 ping-pong 低延迟）。

### Ping-pong 延迟不变

快速路径保证小批量（1-3 帧）不走 ring。ping-pong 8µs 与 Phase 4 持平。P999 26µs 比以前略好。

---

## 火焰图对比

### Pipeline（IO+Matching 线程）

| 分类 | Phase 4 | Phase 5 rev2 | 变化说明 |
|:----|:-------:|:------------:|---------|
| 撮合逻辑 | ~59% | ~60% | 占比相近，仍是瓶颈 |
| send 路径 | ~18.8% | ~17.5% | Send 线程分担后 IO 侧 send 下降 |
| recv 路径 | ~8% | ~8% | 不变 |
| epoll_wait | ~0% | ~0% | 多连接消除空转 |
| 管理开销 | ~14% | ~8% | 去掉 resp_buf/epoll_ctl，省下的 |

---

## 硬件事件

| 事件 | Phase 4 | Phase 5 rev2 | 变化 |
|:----|:-------:|:------------:|:----:|
| IPC | 1.39 | 1.28 | -8%（双线程跨核）|
| cache-misses | 9,390,601 | 6,575,277 | -30%（IO 路径更短）|
| L1-dcache-load-misses | 17,636,359 | 18,405,322 | +4%（跨核影响小）|
| L2-load-misses | 1,807,453 | 1,614,898 | -11% |
| syscalls:sendto | 31,308 | 81,058 | +2.6×（发送粒度变化）|
| syscalls:recvfrom | 105,649 | 105,597 | 不变 |
| syscalls:read | 0 | 30 | ring 路径触发了 Send 唤醒 |
| ctx/s | 54,100 | 50,595 | 持平 |

---

## 参数选择

| 参数 | 值 | 依据 |
|------|:--:|------|
| RING_SIZE | 1,048,576 (1MB) | 覆盖 ~0.75ms @ 19M QPS 抖动 |
| fast path 阈值 | 100 帧 | ping-pong（≤3 帧）直达，pipeline（≥128 帧）切 ring |
| fast path EAGAIN 阈值 | 500 spins | 确保 loopback 上不误切 ring |
| Send 线程唤醒 | eventfd blocking read | 比 epoll 简化，一个 syscall 完成 |

---

## 后续方向

| 阶段 | 内容 | 目的 |
|:----|------|------|
| Phase 6 | 内存池 + 平坦数据结构 | 解决撮合逻辑的 cache miss 瓶颈 |
| Phase 7 | io_uring | send/recv syscall 批量化 |
