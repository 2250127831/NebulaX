# NebulaX 性能监测报告 — Phase 7 io_uring

**日期：** 2026-05-24 | **系统：** Ubuntu 22.04 | **网络：** 127.0.0.1 loopback TCP

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

与前序保持一致：Pipeline 模式 50M 条 × 3 轮（4 连接平分），Pingpong 模式 500K 条 × 3 轮（单连接串行）。命令序列：50% NEW + 25% CANCEL + 25% BOOK，价格区间 10000~14999 重叠。每条测试采集两组数据：无 perf 的纯净延迟 + perf stat 的硬件事件。

**QPS 计算说明：** Pipeline 采用 `total_cmds / max_wall_us`（总命令数 / 最慢 worker 耗时）。

---

## 架构变更

IO+Matching 线程的事件循环从 `epoll_wait` + `recv()` 替换为 `io_uring`。

```
Phase 6                               Phase 7
───────                               ───────
IO+Matching (core 6)                  IO+Matching (core 6)
epoll_wait + recv()循环                io_uring_enter(submit+wait)
  → match                               → match
  → push ring + write(eventfd)          → push ring + write(eventfd)
                                       

Send (core 7)                         Send (core 7)
read(eventfd) + send from ring        read(eventfd) + send from ring
                                      
```

### 为什么 Send 线程不动

SPSC ring 的 `read_acquire` 返回 ring 内部缓冲区指针，`send(fd, ptr)` 直接从 ring 发送，线程间零拷贝。io_uring 的异步 `IORING_OP_SEND` 要求 buffer 在内核完成前保持稳定——但 IO 线程同时持续 `push` 新数据进 ring，可能覆盖未完成的 send 区域。两种内存可见性模型互斥。强行用 io_uring 异步 send 要么需要拷贝（破坏零拷贝），要么需要同步等待 CQE（失去异步收益）。recv 侧不存在这个问题——recv buffer 是独立分配的，io_uring 直接往里写即可。

### 新增文件

| 文件 | 说明 |
|:----|------|
| `include/io_uring_poller.h` | io_uring 封装类（120 行），管理 SQ/CQ + accept/recv SQE + 固定缓冲区池（64 × 4KB） |

### 修改文件

| 文件 | 变更 |
|:----|------|
| `include/tcp_server.h` | 移除 `epoll_fd_`，新增 `IoUringPoller poller_`；`handleAccept`→`onAccept`，`handleRead`→`onRecv` |
| `src/tcp_server.cpp` | `start()` 事件循环重写；解析/匹配逻辑保持与 Phase 6 一致 |
| `CMakeLists.txt` | 新增 `liburing` 依赖 |

---

## 对比数据

### 关键指标一览

#### Pipeline（50M × 3，4 连接，纯净无 perf）

| 指标 | Phase 6 (epoll) | Phase 7 (io_uring) | Δ |
|:----|:------------:|:-------:|:---|
| QPS | 12.3M | **13.6M** | **+10%** |
| avg 延迟 | 244ms | **40ms** | **-83%** |
| P50 | 34ms | 41ms | +22% |
| **P99** | 909ms | **55ms** | **-94%** |
| **P999** | 1,428ms | **58ms** | **-96%** |

Pipeline 尾延迟降低 94%~96%。io_uring 每次 CQE 等粒度处理消除了 epoll ET drain 循环的批处理拥堵——Phase 6 某次 epoll 迭代可能独占 CPU 处理大量积压数据，导致其他连接的响应被长时间阻塞。Phase 6 的 P999 高达 1.4 秒，Phase 7 压缩到 58ms。

#### Pingpong（1M × 3，单连接，纯净无 perf）

| 指标 | Phase 6 (epoll) | Phase 7 (io_uring) | Δ |
|:----|:------------:|:-------:|:---|
| QPS | 181K | **197K** | **+8%** |
| avg | 5-6µs | **5µs** | ~-1µs |
| P50 | 5µs | 5µs | 持平 |
| P99 | 7µs | **6µs** | **-14%** |
| P999 | 8µs | **7µs** | **-13%** |

单连接串行模型下 epoll 和 io_uring 的往返路径都很短，差距在 1µs 级别。

### 硬件事件对比

#### Pipeline 持续负载下（perf stat，脚本采集）

| 事件 | Phase 6 | Phase 7 | 说明 |
|:----|:-------:|:-------:|------|
| cycles | 93.4B | **73.7B** | -21%，io_uring 内核路径更高效 |
| instructions | 105.6B | **77.2B** | -27%，砍掉了 epoll fd 扫描和 recv 调用链 |
| IPC | 1.13 | 1.05 | -7%，context-switches 增加打断流水线 |
| cache-misses | 576M | **254M** | -56%，io_uring 共享内存环避免 epoll fd set 缓存污染 |
| L1-dcache-load-misses | 627M | 1,190M | +90%，io_uring 内核路径访问更多内存 |
| LLC-load-misses | 149M | **71M** | -53%，总缓存命中率改善显著 |
| sendto | 7.9M | **1.2M** | -85%，更均匀的处理节奏减少分散 send |
| context-switches | 58K | **1.13M** | +19×，等粒度 CQE 等待触发更多自愿切换 |

#### Pingpong 下（perf stat，脚本采集）

| 事件 | Phase 6 | Phase 7 | 说明 |
|:----|:-------:|:-------:|------|
| cycles | 26.3B | **24.1B**  | -8% |
| cache-misses | 4.7M | **4.8M**  | 持平 |
| IPC | 1.68 | 1.67  | 持平 |

Pingpong 的单连接简单访问模式使得 epoll 的 fd set 扫描开销不突出，io_uring 的优势主要体现在 syscall 合并（砍掉 recvfrom）而非缓存改善。

Phase 7 pingpong 数据含 perf 干扰（avg=8µs vs 纯净 5µs），因此与 Phase 6 的 perf-stat 数据（含相同干扰）直接可比。

### syscall 计数（perf stat tracepoint）

| syscall | Phase 6 pp | Phase 7 pp | Phase 6 pl | Phase 7 pl |
|:----|:---:|:---:|:---:|:---:|
| `epoll_wait` | 1,500,604 | **0** | 31,593 | **0** |
| `recvfrom` | 4,501,803 | **0** | 1,291,805 | **0** |
| `io_uring_enter` | 0 | 1,500,605 | 0 | —  |
| `sendto` | 1,500,600 | 1,500,600 | 7,885,250 | **1,217,377** |

  脚本的 perf stat 未采集 `io_uring_enter` tracepoint。之前手动采集结果：327,020（pipeline 50M × 3，21.6s）。

Pipeline 下 `sendto -85%` 是间接收益：io_uring 的等粒度 CQE 处理使响应更自然地攒批到一次 `send()`，而非分散为多次小 send。

### CPU 热点分布（Pipeline 火焰图，脚本采集）

| 热点 | Phase 6 | Phase 7 | 说明 |
|:----|:-------:|:-------:|------|
| processNewOrder | 37.0% | 47.6% | 占比上升因分母（总 CPU）变小 |
| addOrder | 19.6% | 24.2% | 同上 |
| matchSellOrder | 6.4% | 9.8% | 同上 |
| IO 路径（epoll/io_uring） | ~15% | **~7%** | io_uring 砍掉 8pp 内核开销 |

撮合逻辑占比"上升"是好事——io_uring 把 IO 路径的开销从 15% 压到 7%，省出来的 CPU 预算自动分配给撮合逻辑。瓶颈更纯了。

## 结论

io_uring 替换 epoll 在 recv 路径上带来了三个维度的改善：

1. **延迟可预测性。** Pipeline P99 从 909ms 降至 55ms（-94%），P999 从 1,428ms 降至 58ms（-96%）。Phase 6 的 epoll ET drain 循环在 4 连接并发下会产生长达 1.4 秒的尾延迟——某次 epoll 返回大量积压数据时，该连接独占 CPU drain 所有数据，其他连接的响应被长时间阻塞。io_uring 的等粒度 CQE 处理（每次返回一个 4KB 数据块然后立即 re-arm）彻底消除了这种批处理拥堵。

2. **CPU 效率。** Total cycles -21%，cache-misses -56%。epoll 每次 `epoll_wait` 需要遍历内核的 interest list 扫描就绪 fd——这个操作在多连接下把大量 epoll_event 数据拉到 L2/L3 cache 中，造成显著的缓存污染。io_uring 的 SQ/CQ 是预注册的共享内存环，内核直接往指定的 CQE 槽写入完成事件，无需扫描、无需额外内存分配。

3. **syscall 批量化。** IO 路径 syscall 减少 75%。Pipeline 下 sendto 间接减少 85%——io_uring 的均匀处理节奏使响应更自然地攒批到一次 `send()`，而非分散为多次小 send。

Pingpong 的单连接串行模型下差距很小（5µs vs 6µs）——单连接时 epoll 的 fd set 扫描开销本就微不足道。io_uring 的价值随并发度放大。

代价：context-switches 增加 19 倍（io_uring_submit_and_wait 每次等待一个 CQE 触发一次自愿切换），L1 misses 增加 90%（io_uring 内核路径访问更多 SQE/CQE 管理结构）。Pipeline P50 从 34ms 升至 41ms——io_uring 每次只拿一个数据块就处理，epoll 的一次性 drain 在数据量适中时更高效。但这些代价换来了 P99 -94% 和 P999 -96%，对于低延迟系统是正确的取舍。

### 固定缓冲区（IORING_REGISTER_BUFFERS）

IoUringPoller 构造时预注册 64 个 4KB 缓冲区（`io_uring_register_buffers`）。每个 ConnContext 从池中分配一个，recv SQE 通过 `sqe->buf_index` 指向注册缓冲区，内核免去每次 recv 的 `get_user_pages` 页表操作。实测 QPS 无显著变化（13.87M vs 13.9M）——recv 路径仅占 ~7% CPU，其中页表 pin 只是很小一部分。这项优化属于"零成本完善"：代码量 ~30 行，不增加复杂度，无关场景下不倒退。

### 数据采集说明

本报告的 Phase 6 vs Phase 7 对比数据均为**回退代码后同条件实测**，且采用新测量方式：先跑纯净版（无 perf 干扰）采集延迟/QPS，再用 perf stat 单独采集硬件事件。旧报告（memory_pool.md 等）的数据是在 perf 后台采样下同时采集延迟和硬件事件——perf 对 pingpong 影响显著（5µs 的 RTT 中 perf 占 ~3µs），因此旧报告的延迟/QPS 数字与本报告不可直接对比。

**io_uring 的价值随并发度放大。** 单连接 pingpong 下改善温和（syscall -75% 但延迟差距 ~1µs），4 连接 pipeline 下全面超越 epoll。如果目标是高并发、低延迟系统——io_uring 是正确选择。

### 后续方向

| 阶段 | 内容 | 目的 |
|:----|------|------|
| Phase 8 | 撮合引擎热点优化 | processNewOrder 仍占 47.6%，cache line 优化空间大 |
