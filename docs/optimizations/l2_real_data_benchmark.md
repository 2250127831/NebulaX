# NebulaX 真实行情回放报告 — Phase 9 L2 实测

**日期：** 2026-06-18 | **系统：** Ubuntu 22.04（VM） | **内核：** 6.8.0

## 系统配置

| 组件 | 规格 |
|------|------|
| CPU | 12th Gen Intel Core i9-12900HX（VM 分配 8 核） |
| 内存 | 8 GB |
| 内核 | 6.8.0-124-generic |
| 绑核 | 服务端 IO core 6（P-core），Send core 7（P-core），客户端 core 5（P-core） |

---

## 背景

Phase 7~8 的基准测试（benchmark_client.cpp）使用均匀分布的价格区间（10000~14999），订单在新价格区间上搓合率稳定。但真实 L2 行情数据具有完全不同的分布特征——价格离散、非均匀、跨度大，这些特征在 OrderBook 上产生的压力模式与均匀价格完全不同。

本报告使用 2026-06-17 沪深 20 只股票的逐笔成交数据，覆盖三个阶段：

1. **基准验证**：原始服务端代码对真实数据的处理能力
2. **回归发现**：增量改动引入的 bug 定位过程
3. **修复验证**：修复后服务端在真实数据上的表现

---

## 数据源

| 项目 | 数值 |
|------|------|
| 数据源 | akshare stock_zh_a_tick_tx_js（腾讯财经） |
| 标的 | 平安银行、贵州茅台、招商银行、万科 A 等 20 只沪深股票 |
| 原始逐笔成交 | 69,633 条 |
| 生成订单（含 ~20% 撤单） | 80,217 笔 |
| CSV 文件 | data/l2_replay.csv（2.1 MB） |

生成逻辑：按时间顺序读取逐笔数据，每笔成交生成一条 NEW 订单，再按概率（~20%）从活跃订单中随机选一条 CANCEL。连续价格作为 side 和 price 的随机种子。

---

## 测试方法

`scripts/l2_replay.py` 一次性将全部订单序列化为 BinaryCommand 字节流，分批发送（每批 4000 笔），同步接收响应。单连接全双工。

---

## 结果

原始服务端代码完整处理 80,217 笔真实离散价格订单，**单连接 100% 完成**，峰值 5.9M QPS。OrderBook 在随机不撮合积累场景下表现正常。

全量修改版（WAL + Logger + 新连接管理）约 700 笔后断连，后续重连每次仅 0-2 笔。二分法定位过程：

```
全量修改版 → FAIL
  ├── 回退 order_pool / order_book / order_map 改 → 仍 FAIL
  ├── 回退 closeConnection (pending_closes_ → 立即回收) → 仍 FAIL
  ├── 回退 submit_and_wait_timeout → submit_and_wait() → PASS ✅
  └── 确认 root cause: timeout SQE user_data 未初始化
```

修复后 Python 回放仍为 80,217/80,217 单连接完成，恢复原始稳定性。

---

## Bug 根因

### 问题

`IoUringPoller::submit_and_wait_timeout()` 添加 timeout SQE 时没有调用 `io_uring_sqe_set_data()`。

```cpp
// 改动前的代码（有 bug）
int submit_and_wait_timeout(uint64_t timeout_ms) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return submit_and_wait();
    struct timespec ts = { .tv_sec = timeout_ms / 1000, ... };
    io_uring_prep_timeout(sqe, &ts, 1, 0);
    // ← 遗漏: io_uring_sqe_set_data(sqe, ...)
    return io_uring_submit_and_wait(&ring_, 1);
}
```

SQE 从内核回收池取出时，`user_data` 字段保留着上次使用的值（可能来自上一次的 `submit_recv(fd=5)`，user_data 残留 `5`）。内核完成 timeout 操作后返回 CQE，`cqe->user_data` = 残留的 fd 值。

### 触发链

`process_cqes` 根据 user_data 分发 CQE：

```
cqe->user_data = 5  (残留)
cqe->res = 0       (timeout 成功)

fd=5 ≠ server_fd  → 走 recv 分支
  → on_recv(5, 0)
    → conns_.find(5) 命中
    → onRecv(conn, 0)
      → bytes_read <= 0 → closeConnection(conn)
      → 无辜连接被关闭
```

timeout 每 500ms 触发一次。重启后 SQE slot 轮转，命中哪个 fd 不固定——所以断连时的订单数在不同测试轮次中略有差异（701、766、966），取决于 SQE ring 的分配时序。

### 为什么之前的压测没触发

| 压测 | 服务端 | 结果 |
|------|--------|------|
| benchmark_client（3.56M QPS） | 原始代码 `submit_and_wait()` | ✅ 无 timeout SQE，bug 不存在 |
| Python smoke_test（210K 笔） | 修改代码 `submit_and_wait_timeout` | ✅ 全速 pipeline < 500ms 跑完，timeout 不触发 |
| C++ l2_bench（80K 笔） | 修改代码 `submit_and_wait_timeout` | ❌ 串行 send/recv 运行 ~1 秒，timeout 稳定触发 |

benchmark_client 跑的是原始服务端（无 timeout 逻辑），smoke_test 跑得太快 timeout 来不及开枪。l2_bench 的串行模式运行时间长，恰好成为触发条件。

### 修复

```
// io_uring_poller.h
int submit_and_wait_timeout(int server_fd, uint64_t timeout_ms) {
    ...
    io_uring_prep_timeout(sqe, &ts, 1, 0);
    io_uring_sqe_set_data(sqe, (void*)(uintptr_t)server_fd);  // 新增
    ...
}

// process_cqes 同时调整：accept 判断从 res>=0 改为 res>0
// timeout 的 res=0 不会触发 on_accept
```

两处改动共 2 行有效代码。

### 经验教训

**io_uring 的 SQE 复用语义需要显式初始化所有字段。** `io_uring_prep_*` 系列函数仅设置特定于该操作的字段，`user_data` 不在其内。不调用 `io_uring_sqe_set_data()` 就提交 SQE，等价于使用未初始化变量的残留值——在单线程循环中，这个残留值恰好是最近一次 recv 操作的 fd，看起来"有规律"，实际上是非确定性 bug。

---

## 关键指标横比

| 指标 | 原始服务器 | 改动版（有 bug） | 修复后 |
|------|:----------:|:----------------:|:------:|
| L2 80K 完成 | 100% | ~1% | 100% |
| 单连接完成 | ✅ | ❌ 需 100+ 次重连 | ✅ |
| QPS（串行） | 72,480 | ~700 | 70,946 |
| QPS（Python pipeline） | 5.9M | — | — |

修复对性能无影响（QPS 差异在噪音范围内）。

---

## 数据文件

L2 真实行情数据：`data/l2_replay.csv`

格式：`t,type,side,price,qty,uid`
- t: 时间偏移（秒）
- type: NEW / CANCEL
- side: 1=买 / 2=卖
- price: 价格（分）
- qty: 数量（股）
- uid: 用户 ID

---

## 脉络

| Phase | 内容 | 参考文档 |
|-------|------|----------|
| 7 | io_uring 双线程无锁架构 | [io_uring.md](io_uring.md) |
| 8 | WAL + 共享内存 + 可靠性工程 | [reliability_engineering.md](reliability_engineering.md) |
| **9** | **真实行情回放 + timeout SQE bug 修复** | **本报告** |
