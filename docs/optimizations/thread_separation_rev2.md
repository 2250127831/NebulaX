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

**QPS 计算说明：** 采用 `total_cmds / max_wall_us`（总命令数 / 最慢 worker 耗时）。多 worker 场景下各 worker 起止时间有差异，`Σ(count_i / wall_i)` 求和会导致 QPS 虚高。

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

**Ring 设计：** 纯字节缓冲区（1MB），push/pop 内部自动处理 wrap。`RSP_HEADER`（type 0x87）做帧同步。read_acquire 返回 ring 内部指针直接 send，零拷贝。

**自适应快速路径：** 响应帧 ≤ 100 时 IO 线程直接 send。ping-pong 走快速路径保持 8µs，pipeline 走 ring 激活双线程。

**通知机制：** eventfd blocking read。批粒度通知。

---

## 对比数据

### 关键指标一览

| 指标 | Phase 3 | Phase 4 | Phase 5 rev2 |
|:----|:-------:|:-------:|:------------:|
| Pipeline burst (500K) | 1.95M | 7.4M | **8.5M** |
| Pipeline sustained (50M)¹ | 2.4M | 4.1M | **7.2M** |
| Ping-pong avg | 7µs | 8µs | **8µs** |
| IPC（burst）| 2.00 | 1.30 | 1.28 |
| IPC（sustained）| — | 0.64 | **0.99** |

Phase 5 rev2 相比 Phase 4：burst +15%，sustained **+76%**。持续负载下差距远大于 burst，原因是 Phase 4 的 resp_buf 管理在长跑中导致 IPC 从 1.30 跌至 0.64，Phase 5 rev2 稳定在 0.99。

¹ Phase 3 sustained 为 blocking TCP 单连接实测值，代码对应 commit d647799。

### 逐轮明细

#### Pipeline（500K burst）

| 轮次 | Phase 4 QPS | Phase 5 rev2 QPS |
|:----|:----------:|:----------------:|
| 1 | 6,582,238 | 8,467,544 |
| 2 | 7,422,582 | 8,226,120 |
| 3 | 8,206,408 | 8,841,889 |
| **均** | **7,403,743** | **8,511,851** |

#### Pipeline（50M sustained）

| 轮次 | Phase 4 QPS | Phase 5 rev2 QPS |
|:----|:----------:|:----------------:|
| 1 | 4,371,021 | 7,513,994 |
| 2 | 3,592,313 | 7,333,043 |
| 3 | 4,471,429 | 6,766,662 |
| **均** | **4,144,921** | **7,204,566** |

#### Ping-pong

| 轮次 | avg | P50 | P99 | P999 | QPS |
|-----|----:|----:|----:|----:|----:|
| Phase 4（参照）| 8µs | 8µs | 11µs | 30µs | 123,221 |
| Phase 5 rev2 1 | 8µs | 8µs | 12µs | 25µs | 119,732 |
| Phase 5 rev2 2 | 9µs | 8µs | 12µs | 26µs | 117,212 |
| Phase 5 rev2 3 | 8µs | 8µs | 11µs | 26µs | 120,236 |
| **rev2 均** | **8µs** | **8µs** | **12µs** | **26µs** | **119,060** |

---

## 火焰图（Sustained 50M）

Pipeline 负载下 Phase 5 rev2 的 CPU 分布：

| 分类 | Phase 4（50M）| Phase 5 rev2（50M）| 说明 |
|:----|:------------:|:-----------------:|------|
| 撮合逻辑 | ~60% | ~65% | 瓶颈仍为 cache miss |
| send 路径 | ~3% | ~6% | Phase 4 被 resp_buf 管理拖垮 |
| recv 路径 | ~8% | ~8% | 不变 |
| 管理/碎片 | ~29% | ~21% | Phase 4 的 memmove/epoll_ctl 开销 |

持续负载下 Phase 4 多出 ~8% 的管理开销（`memmove` / `epoll_ctl` / `vector` 操作），对应 IPC 从 burst 1.30 跌至 0.64。

---

## 硬件事件

| 事件 | Phase 4 | Phase 5 rev2 |
|:----|:-------:|:------------:|
| IPC（burst 500K）| 1.30 | 1.28 |
| IPC（sustained 50M）| 0.64 | **0.99** |
| cache-misses（burst）| 9,390,601 | 6,575,277 |
| sendto/run（burst）| 31,308 | 81,058 |
| recvfrom/run | 105,649 | 105,597 |
| ctx/s（sustained）| 614 | 893 |

---

## 后续方向

| 阶段 | 内容 | 目的 |
|:----|------|------|
| Phase 6 | 内存池 + 平坦数据结构 | 解决撮合逻辑 cache miss 瓶颈 |
| Phase 7 | io_uring | send/recv syscall 批量化 |
