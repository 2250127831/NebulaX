# NebulaX 工程化报告 — Phase 7~8 可靠性工程

**日期：** 2026-06-18 | **系统：** Ubuntu 22.04（VM） | **内核：** 6.8.0

## 系统配置

| 组件 | 规格 |
|------|------|
| CPU | 12th Gen Intel Core i9-12900HX（VM 分配 8 核） |
| 内存 | 8 GB |
| 内核 | 6.8.0-124-generic |
| 存储 | /dev/shm（tmpfs），/tmp（ext4） |

---

## 背景

Phase 7（io_uring）后的 NebulaX 已具备双线程无锁架构、SPSC ring 零拷贝发送、二进制协议等性能基座。但生产级系统需要在"崩了怎么办"上给出答案。

本阶段目标：**任何崩溃不丢数据，系统自恢复。**

---

## 架构变更

### 1. 结构化日志

Phase 7 之前所有日志用 `write(STDERR)` 直写，IO 线程同步阻塞。替换为独立日志系统：

```
IO/Matching (任何线程)          日志消费线程
  → MPSCRing::alloc()             → clock_gettime 时间戳
  → vsnprintf 格式化              → dprintf(log_fd, ...)
  → ring::commit() + eventfd 通知 → ring::consume()
  （纯内存操作，无 IO）            （IO 操作集中到消费线程）
```

**MPSC 环形缓冲区：** 多生产者 CAS 占槽 + seq 两阶段提交。预分配 4096 个固定槽位（256 bytes/槽），无 malloc 无锁。

**分级：** INFO/WARN/ERROR/FATAL，阈值过滤。热路径输出级别直接 return 跳过 CAS。

**每千笔汇总：** IO 线程主循环每 1000 笔 new_orders 打汇总线（orders/trades/pool%），替代 per-order 逐条日志。

### 2. WAL + 崩溃恢复

```
正常路径:
  收到 NEW/CANCEL → WalWriter::append(mmap memcpy) → 执行撮合 → 回复
                      ↑ 零 syscall，纯内存拷贝

崩溃路径:
  SIGSEGV → CrashGuard: fdatasync(WAL fd) + _exit
                     ↑ 不做 shm_unlink，共享内存保留

恢复路径场景 A（共享内存完好 /dev/shm/）:
  → mmap 直接读 OrderPool → 遍历 OPEN 订单 → 重建 bids_/asks_
  → 恢复时间 O(N)，N=在册订单数

恢复路径场景 B（共享内存丢失 rm / reboot）:
  → 加载 checkpoint → WAL 幂等回放（只存输入，不存输出）
  → 同输入序列 ⇒ 同最终状态
```

**WAL 设计：** 单文件 512MB 环形 mmap。每条 NEW/CANCEL 先 `base_[pos] = entry` 再执行。`needCheckpoint()` 在每写满一轮时触发 fork 子进程 dump 共享内存快照。

**幂等回放原理：** WAL 只存输入命令（NEW/CANCEL），不存撮合结果。回放时重做所有输入，撮合引擎本身决定输出——相同输入序列必定导向相同状态。

### 3. 共享内存订单池

OrderPool 从进程堆迁移到 `shm_open` + `mmap(MAP_SHARED)`：

```
进程崩溃 → 内核清理 fd + 解除 mmap
          → /dev/shm/nebulaX_book 文件仍在（未调 shm_unlink）
重启 → shm_open 同名文件 → mmap → 数据完好
```

变更影响：OrderPool 构造函数接受外部指针，`OrderBook::pool_` 改指针，`OrderMap` 仍在进程堆（崩溃后从共享内存 OrderPool 重建）。

### 4. 连接管理

| 功能 | 实现 |
|------|------|
| 死连接检测 | TCP keepalive（10s 探测，5s 间隔，3 次失败断连） |
| 关闭竞态 | RSP_CLOSE 带 ack 指针（atomic\<bool\>*），Send 线程 close 后写回 |
| 空闲超时 | io_uring POLL_FIRST + linked timeout 由内核触发 |
| CQE 差异化 | EAGAIN→重提 recv，ECONNRESET/EPIPE→正常断连，其他→日志+断连 |

### 5. 监控与可观测性

Per-thread 计数器（`IOCounters` / `SendCounters`）通过 `shm_open` 暴露：

```
共享内存 /dev/shm/nebulaX_metrics:
  IO 线程: recv_frames, new_orders, cancels, trades, errors, order_pool_used
  Send 线程: send_batches, send_bytes, send_zc_ok/fail
```

心跳检测：IO 线程每 tick 更新 `io_heartbeat`，Send 线程 `poll(wake_fd, 1s)` 维持心跳。IO 线程 3 个 tick 内检测到 send 心跳停滞 → eventfd 唤醒 → 接管发送。

---

## 测试

| 类型 | 用例数 | 覆盖 |
|------|--------|------|
| 单元 | 33 | SPSC/MPSC/OrderPool/OrderBook/Protocol/自成交防护/OrderMap 溢出 |
| 集成（自动化） | 10 场景 | build → 下单 → SIGSEGV → shm 恢复 → WAL 恢复 |
| 压力（Python） | 2 阶段 | 铺底 500K + 搓合 200K |

### 崩溃恢复验证

```
测试 1: SIGSEGV → 共享内存存活
  → 发 100 笔 → kill -SEGV → 重启 → 100 笔恢复 ✅

测试 2: 共享内存丢失 → WAL 回放
  → 发 50 笔 → 正常关闭 → rm /dev/shm → 重启 → WAL 回放恢复 50 笔 ✅
```

### WAL 吞吐影响

Python 压测 4 连接并发：

| 阶段 | 笔数 | 耗时 | QPS |
|------|------|------|-----|
| 铺底（无撮合） | 10K | 0.17s | 60K |
| 爆发撮合 | 10K | 0.003s | 3.5M |

铺底阶段需要写 WAL + 入 OrderPool + 插入 bids_，60K QPS 受限于 Python 单连接串行（瓶颈在 send/recv 往返，非服务端）。爆发撮合 3.5M QPS 体现匹配路径实际吞吐。

---

## WAL 内存开销

| 组件 | 大小 | 说明 |
|------|------|------|
| WAL 文件 | 512MB | /tmp/nebulaX_wal.dat，mmap 环形 |
| 共享内存订单池 | 256MB | 4M × 64B Order 结构 |
| 共享内存 TradePool | 84MB | 1M × ~84B TradeEntry |
| metrics 共享内存 | 112B | IOCounters + SendCounters |

---

## 关键决策

**1. WAL 为什么用 mmap 不用 write：** 每条命令 append 是 memcpy（约 10ns），write 系统调用约 200ns+。mmap 使 WAL 不成为热路径瓶颈。

**2. 为什么不用独立订单簿进程：** 同进程内调用 OrderBook 接口零开销，独立进程需要 IPC（shm 队列或 RPC）增加延迟。当前架构崩溃恢复依赖共享内存＋WAL，隔离性对 demo 项目足够。

**3. 恢复路径为什么不全部走 WAL：** 共享内存直读 O(N) 遍历 OPEN 订单，N=在册数（通常几千）。WAL 回放需要重放从开始到现在的所有操作（百万级），慢两个数量级。

**4. Checkpoint 为什么用 fork：** fork 的 COW 语义给子进程一个瞬间一致的快照，父进程零阻塞继续匹配。子进程写磁盘期间，匹配线程不暂停。

---

## 脉络

Phase 7（io_uring）将系统从多线程 epoll 推进到双线程无锁架构。Phase 8（可靠性工程）在性能基座上叠加：

| 模块 | 解决 | 代价 |
|------|------|------|
| WAL | 崩溃丢数据 | +512MB 磁盘，每条操作 +10ns（mmap memcpy） |
| 共享内存 | 进程间状态持久化 | +340MB 共享内存 |
| MPSC 日志 | IO 线程沾系统调用 | +1 消费者线程，+256KB 缓冲区 |
| 心跳检测 | Send 线程死锁不可知 | 每 tick +2 次原子读写 |
| OrderMap 溢出 | 哈希退化 O(n) | +8MB bucket_len 数组，+std::map 兜底 |

所有代价均在 demo 项目可接受范围内。WAL 的 10ns 在 Python 实测中不可测量（60K~3.5M QPS 量级），只有 C++ benchmark 在千万 QPS 下才需要关注。
