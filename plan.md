# NebulaX 工程化推进计划

## 已完成

- [x] 优雅关闭（SIGTERM handler + drain + io_shutdown_done 标志）
- [x] Ring 背压（空间不足时自动降级 plain send）
- [x] 停机快照（优雅关闭时保存订单簿，重启后恢复）
- [x] CQE 错误差异化（EAGAIN 重试、ECONNRESET/EPIPE 正常断连、其他日志）
- [x] 连接管理（TCP keepalive 死连检测 + ack 关闭竞态修复）
- [x] 监控指标（Per-thread 计数器 + shm_open 共享内存 + read_metrics.py 采集脚本）
- [x] 结构化日志（MPSC 环形缓冲区 + 分级时间戳日志 + 消费者线程）

## 待完成

### 1. WAL + 崩溃恢复 + 线程存活检测（~4天）

**目标：** 任何崩溃不丢订单数据，Send 线程崩后恢复未发送的响应。

**架构：**

```
共享内存 (shm_open /dev/shm/nebulaX_book):
  OrderPool[N]      ← 活跃在册订单（完成/取消后 deallocate）
  TradePool[M]      ← 近期成交环形缓冲（带 response_seq）
  Metadata          ← next_id、心跳计数器、wal_seq

WAL (mmap /tmp/nebulaX_wal.dat):
  每条 NEW/CANCEL 先追加写入 WAL，512MB 循环覆盖
  满时 checkpoint（fork 子进程 dump 共享内存）

崩溃恢复:
  ① 共享内存仍在 → 直接读 OrderPool → 重建 bids_/asks_
  ② 共享内存丢失 → 加载 checkpoint → WAL 幂等回放
```

**关键机制：**
- WAL 只存输入（NEW/CANCEL），幂等回放
- TradePool 中 response_seq=0 表示未发送，恢复时补推
- 心跳检测：IO 线程 3 个 tick 内 Send 线程无心跳 → 接管发送
- WAL 满时 fork checkpoint；优雅关闭用 saveSnapshot

**文件：**
| 文件 | 改动 |
|------|------|
| `include/order_pool.h` | 支持 mmap 外部存储 |
| `include/trade_pool.h` | 新建（Trade 环形缓冲） |
| `include/wal.h` | 新建（WalEntry + 幂等回放） |
| `src/wal.cpp` | 新建（mmap append + checkpoint + 回放） |
| `include/order.h` | 无变化（保持 64 字节） |
| `include/matching_engine.h` | 新增 wal_、trade_pool_ 成员 |
| `src/matching_engine.cpp` | 每条操作先写 WAL；成交写 TradePool |
| `src/order_book.cpp` | removeOrder 仍 deallocate |
| `src/main.cpp` | 共享内存 init + 心跳 + 崩溃 handler + 恢复 |
| `include/crash_guard.h` | 新建（SIGSEGV handler: msync + _exit） |

### 2. 测试补充（持续）

- 单元测试（组件边界条件）
- 崩溃恢复验证（SIGSEGV → 重启 → 数据完好）
- WAL 回放验证（rm /dev/shm → checkpoint + WAL 重建）
