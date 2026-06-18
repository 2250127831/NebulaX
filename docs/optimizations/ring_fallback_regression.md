# NebulaX 回归修复与性能验证报告 — Phase 10 ring 满降级 + 客户端修复

**日期：** 2026-06-18 | **系统：** Ubuntu 22.04 | **内核：** 6.8.0

## 系统配置

| 组件 | 规格 |
|------|------|
| CPU | 12th Gen Intel Core i9-12900HX（Alder Lake，8 P-core + 16 E-core） |
| 内存 | 31 GB |
| 内核 | 6.8.0-117-generic |
| 绑核 | IO+Matching core 4（P-core），Send core 5（P-core），Client core 5（P-core） |

---

## 背景

### 问题 1：Pipeline 模式 benchmark 卡死

benchmark_client pipeline 模式在 4 连接并发时完全无输出，pingpong 模式正常。使用 Phase 7 客户端代码（diff 仅 TOTAL_CMDS 不同）同样卡死，说明问题出在服务端。

### 问题 2：CANCEL 被当作 BOOK 发送

Pipeline 模式下 sender 线程把所有非 NEW 命令（type 1=CANCEL, type 2=BOOK）一律发成 `CMD_BOOK`。50M 命令的 50% 是 NEW（25M），OrderPool 只有 4M 条目，pool 很快就撑爆。

---

## 根因

### 回归：b15f00b 丢失 ring 满降级

`b15f00b`（io_uring timeout SQE user_data 修复）在修改 `process_cqes` 和 `submit_and_wait_timeout` 的同时，反向掉了 `c3776c1` 引入的 `pushResponses` 改动：

```
c3776c1 (fix: pushResponses 失败时 closeConnection)
  → pushResponses 从 void 改为 bool
  → ring 满时降级直接 send ✅

b15f00b (fix: io_uring timeout SQE)
  → pushResponses 改回 void
  → 移除 ring 满降级，换为死循环 while(free_space < need) { pause() } ❌
```

退化后的代码：

```cpp
// b15f00b 后的代码——ring 满时死循环
while (ring_.free_space() < sizeof(BinaryResponse) + bytes) {
    notifySendThread();
    __builtin_ia32_pause();
}
```

原逻辑（`070382f` / `c3776c1`）：

```cpp
// ring 有空间走 ring，没有就直接 send
if (ring_.free_space() >= sizeof(BinaryResponse) + bytes) {
    // push to ring, notify send thread
} else {
    // direct send（不阻塞 IO 线程）
}
```

### 触发链：prefill 与并发流量争 IO 线程

worker 0 在 prefill 阶段（200 笔顺序收发）的响应被其他 3 个 worker 的 pipeline 流量淹没：

```
worker 0: connect → prefill (200 send/recv) → pipeline
workers 1-3: connect → pipeline (250K cmd each)

服务端 IO 线程：
  1. 收到 workers 1-3 的 750K 命令 → 处理 → 推响应到 ring
  2. ring 满 → pushResponses 死循环等空间
  3. worker 0 的 prefill 响应 CQE 在 CQ ring 中排队
  4. IO 线程被卡在 pushResponses，永远处理不到 → 死锁
```

prefill 提前做完即可避免竞争，也是客户端侧的唯一修复。

---

## 修复

### 服务端：恢复 ring 满降级（tcp_server.cpp pushResponses）

将 fast path（`count ≤ 100 && ring 空`）+ ring spin 替换为无条件判断：

```
判断条件              动作
─────────────────     ──────────────────────────
ring 有足够空间        走 ring → Send 线程异步发送
ring 空间不足          直接 send（spin 上限 500 次）
```

删除了 `while (free_space < need) { pause() }` 死循环。两种路径都带 spin 上限和错误返回，不会阻塞 IO 线程。

### 客户端：prefill 提前做（benchmark_client.cpp）

将 200 笔 resting order 的 prefill 从 worker 0 内部移到启动 4 个 worker 线程之前，用独立连接做。prefill 完成后才启动并发 pipeline，消除 IO 线程竞争。

### 客户端：CANCEL 支持（benchmark_client.cpp）

sender/receiver 线程通过 `std::mutex` 保护的 `vector<uint64_t>` 共享 order_id 队列：

- **receiver**：收到 `RSP_OK` / `RSP_FILLED` 时推 order_id 入队
- **sender**：遇到 `CMD_CANCEL` 时从队列取 order_id，无可用订单时回退 BOOK

---

## 对比数据

### Pipeline（10M 命令，4 连接并行）

| 轮次 | QPS | avg | P50 | P99 | P999 |
|:----|----:|----:|----:|----:|----:|
| 1 | 6,026,075 | 53ms | 54ms | 58ms | 60ms |
| 2 | 5,934,538 | 53ms | 54ms | 60ms | 61ms |
| 3 | 6,232,366 | 58ms | 53ms | 105ms | 106ms |
| **均** | **6,064,327** | **55ms** | **54ms** | **74ms** | **76ms** |

（延迟为 RDTSC 管道延迟，因 send/recv 索引独立计数不反映单命令耗时）

### Pipeline（1M 命令，4 连接并行）

| 轮次 | QPS | avg | P50 | P99 | P999 |
|:----|----:|----:|----:|----:|----:|
| 1 | 5,832,638 | 43ms | 49ms | 59ms | 62ms |
| 2 | 4,940,760 | 47ms | 50ms | 91ms | 95ms |
| 3 | 4,913,087 | 40ms | 45ms | 58ms | 58ms |
| **均** | **5,228,829** | **43ms** | **48ms** | **69ms** | **72ms** |

### Pingpong（1M 命令，单连接串行）

| 轮次 | avg | P50 | P99 | P999 | QPS |
|:----|----:|----:|----:|----:|----:|
| 1 | 6µs | 6µs | 7µs | 25µs | 167,536 |
| 2 | 6µs | 6µs | 8µs | 20µs | 168,216 |
| 3 | 6µs | 6µs | 7µs | 24µs | 168,234 |
| **均** | **6µs** | **6µs** | **7µs** | **23µs** | **167,995** |

Pingpong 延迟未退化，保持 6µs。

### 与历次对比

| 指标 | Phase 7（io_uring）| 当前 |
|:----|:------------------:|:----:|
| Pipeline burst（500K）| 8.5M | — |
| Pipeline sustained（50M）| 7.2M | — |
| Pipeline（10M）| — | **6.1M** |
| Pipeline（1M）| — | **5.2M** |
| Pingpong 延迟 | 8µs | **6µs** |
| Pingpong QPS | 119K | **168K** |

当前 6.1M QPS 低于 Phase 7 7.2M 的差异主要来自 WAL 写入开销（每笔 NEW 追加写 512MB mmap ring buffer）和结构化日志（MPSC ring + 消费者线程），而非 ring 满降级修复本身。

---

## 脉络

| Phase | 内容 | 参考文档 |
|-------|------|----------|
| 7 | io_uring 双线程无锁架构 | [io_uring.md](io_uring.md) |
| 8 | WAL + 共享内存 + 可靠性工程 | [reliability_engineering.md](reliability_engineering.md) |
| 9 | 真实行情回放 + timeout SQE bug 修复 | [l2_real_data_benchmark.md](l2_real_data_benchmark.md) |
| **10** | **ring 满降级恢复 + 客户端 CANCEL/prefill 修复** | **本报告** |
