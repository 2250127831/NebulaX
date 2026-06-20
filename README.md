# NebulaX — Low Latency Exchange Engine

C++ 撮合引擎，基于 io_uring + 双线程无锁架构，配备崩溃恢复、运行时监控和异步日志。

## 架构

![V3 架构图](docs/images/V3架构图.png)

### 数据流

```
Client → [TCP] → io_uring recv
                    ↓
               onRecv → MatchingEngine → pushResponses
                                             ↕ SPSC byte ring
                                        Send 线程 → [TCP] → Client
```

### Matching Engine

Price-Time Priority（价格优先 + 时间优先 FIFO），支持部分成交、撤单。`std::map<price, PriceLevel>` 买卖盘 + OrderMap（链长达到 8 后整体转入 std::map）O(1) 撤单索引。

### 协议

二进制定长帧：命令 32 字节，响应 48 字节。定义见 [protocol.h](include/protocol.h)。

| 命令 | 类型 | 功能 |
|------|------|------|
| CMD_NEW | 0x01 | 下单 |
| CMD_CANCEL | 0x02 | 撤单 |
| CMD_BOOK | 0x03 | 查行情 |

---

## 性能

**Pipeline / Ping-pong：** 12th Gen Intel Core i9-12900HX / 24 核 / 31GB RAM / Ubuntu 22.04 / Linux 6.8.0，IO+Matching core 6、Send core 7、Client core 5（P-core）<br>
**L2 真实行情：** Ubuntu 22.04 VM / 8 vCPU / 8GB RAM / Linux 6.8.0

| 指标 | Pipeline (50M) | Ping-pong (1M) | L2 真实行情¹ |
|:----|:-------------:|:--------------:|:----------:|
| QPS | **13.9M** | 197K | **5.9M** |
| P50 | 40ms | 5µs | 42ms |
| P99 | 51ms | 6µs | 53ms |
| P999 | 56ms | 7µs | 58ms |

L2 数据：2026-06-17 沪深 20 只股票逐笔成交（69,633 条 → 80,217 笔订单含 ~20% 撤单），Python 批量回放。数据文件 `data/l2_replay_20260617.csv`。

---

## 项目结构

```
NebulaX/
├── include/
│   ├── matching_engine.h          撮合引擎
│   ├── order_book.h               买卖盘 + OrderMap 索引
│   ├── order_pool.h               内存池（4M 定长槽，支持 mmap 共享存储）
│   ├── order_map.h                链长达到 8 后整体转入 std::map
│   ├── protocol.h                 二进制协议
│   ├── tcp_server.h               io_uring 服务端
│   ├── io_uring_poller.h          io_uring 封装（固定缓冲区 + 超时处理）
│   ├── spsc_byte_ring.h           SPSC 无锁环形缓冲区
│   ├── wal.h                      WAL 环形 mmap
│   ├── logger.h                   MPSC 无锁异步日志
│   ├── metrics.h                  共享内存计数器
│   ├── crash_guard.h              SIGSEGV/SIGABRT handler
│   └── shutdown_guard.h           优雅关闭
├── src/                           实现
├── benchmark/                     压测客户端
├── scripts/
│   ├── nebulaX_bench.sh           压测 + perf + 火焰图
│   ├── l2_replay.py               真实行情回放
│   └── read_metrics.py            共享内存指标读取
├── data/
│   └── l2_replay_20260617.csv     沪深 20 只股票逐笔数据
├── docs/
│   ├── BENCHMARK.md
│   └── optimizations/             各阶段优化报告
└── profiling/                     火焰图
```

---

## 系统特性

### 崩溃恢复

- **WAL**：512MB 环形 mmap，每条 NEW/CANCEL 先 memcpy 后执行，零 syscall
- **共享内存**：OrderPool（4M × 64B）+ TradePool（1M 成交缓冲）+ 元数据（心跳 + WAL 序号），通过 `shm_open + mmap(MAP_SHARED)` 暴露。进程崩溃不 `shm_unlink`
- **三层恢复**：共享内存直读 → checkpoint + WAL 增量 → WAL 幂等全量回放
- **CrashGuard**：SIGSEGV/SIGABRT/SIGBUS handler 刷 WAL 后 `_exit`

### 连接管理

| 功能 | 实现 |
|------|------|
| 死连接检测 | TCP keepalive（10s 探测 + 5s 间隔 + 3 次失败断连） |
| CQE 差异化 | EAGAIN→重提 recv，ECONNRESET/EPIPE→正常断连 |
| 关闭竞态 | RSP_CLOSE 带 atomic ack，Send 线程确认关 fd 后才释放资源 |
| Send 线程存活检测 | 3 tick 心跳停滞 → eventfd 唤醒重试 |

### 无锁数据结构

- **SPSC 字节环形缓冲区**：单生产者单消费者，无锁读写，read_acquire/read_release 零拷贝
- **MPSC 环形缓冲区**（日志）：多生产者 CAS 占槽 + seq 两阶段提交
- **OrderMap**：bucket 链长达到 8 后整体转入 std::map 防退化
- **OrderPool**：4M 定长槽，O(1) allocate/deallocate，无 malloc

### 可观测性

- **共享内存计数器**（`/dev/shm/nebulaX_metrics`，128 bytes）：IO/Send 双线程指标，16 个 uint64_t，包括 tick_counter（空闲也能看线程存活）
- **SPSC ring 监控**（`/dev/shm/nebulaX_ring`，24 bytes）：实时 tail/head/capacity，支持外部读取
- **MPSC 异步日志**：消费线程集中写盘，IO 线程零阻塞，4 级过滤
- **read_metrics.py**：外部实时采集

---

## Benchmark

```bash
# Pipeline 吞吐（默认）
sudo bash ./scripts/nebulaX_bench.sh

# Ping-pong 延迟
sudo bash ./scripts/nebulaX_bench.sh -r

# L2 真实行情回放
python3 scripts/l2_replay.py 2250

# 读取运行时指标
python3 scripts/read_metrics.py
```

输出：QPS / 延迟分布 / perf 热点 / 火焰图 / IPC / cache-misses / ctx/s

---

## 优化路线

| 阶段 | 内容 |
|:----|------|
| V1 | 纯文本协议基线（69 万 QPS） |
| Phase 3 | 二进制协议 + 批量收发 |
| Phase 4 | epoll ET reactor + 多连接 |
| Phase 5 rev2 | 双线程 + SPSC byte ring + 快速路径 |
| Phase 6 | 内存池 + 平坦数据结构 |
| Phase 7 | io_uring recv + SEND_ZC，recv/send 零拷贝（1,390 万 QPS） |
| Phase 8 | WAL + 共享内存 + 崩溃恢复 + 异步日志 + 监控指标 |
| Phase 9 | 真实行情回放 + 连接可靠性 + ring 满降级回归 |

经过 9 轮迭代，Pipeline QPS 从 69 万提升至 1,390 万（+20 倍），尾延迟从 270ms 降至 56ms（-96%）。每轮使用 perf + 火焰图定位瓶颈。

---

## 环境

- 构建：CMake + g++11（Ubuntu 22.04）+ liburing 2.4
- 压测：C++17，POSIX sockets，taskset 绑核
- 分析：perf dwarf unwind，FlameGraph
