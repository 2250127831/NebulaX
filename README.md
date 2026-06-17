# NebulaX — 低延迟撮合引擎

[![GitHub](https://img.shields.io/badge/GitHub-2250127831/NebulaX-181717?logo=github)](https://github.com/2250127831/NebulaX)

io_uring · epoll · SPSC 无锁队列 · 内存池 · C++17 · perf · CMake

基于价格时间优先的限价订单簿撮合引擎，配备崩溃恢复（WAL + 共享内存）、信号处理、线程存活检测、网络层容错与过载保护，集成运行时指标采集和异步日志。双线程无锁架构 + io_uring 零拷贝，真实行情数据回放 590 万 QPS（2026-06-17 沪深 20 只股票逐笔成交）。

▸ 崩溃恢复：WAL（mmap 环形缓冲区，零 syscall）先写后执行 + SIGSEGV handler fdatasync 后 \_exit 保留共享内存，重启后直读或走 WAL 幂等回放；Checkpoint 用 fork COW 快照子进程写盘，父进程零阻塞

▸ 信号处理 + 线程存活检测，Send 线程无响应由 IO 线程通过 eventfd 唤醒并接管发送，IO 线程崩溃则外部进程管理器 detect 后拉起

▸ 基于 io_uring 自封装事件轮询器，预注册 64 块固定缓冲区消除 recv 数据拷贝，支持 SEND_ZC 零拷贝发送。接收处理和发送分离为两个线程，通过自实现 SPSC 字节环形队列和 eventfd 跨线程通信

▸ 连接管理：TCP keepalive 死连检测（10s 探测 + 3 次失败断连）、RSP_CLOSE 带 atomic ack 关闭竞态消除、区分 EAGAIN/ECONNRESET/EPIPE 做差异化处理

▸ 运行时指标采集（共享内存 IOCounters/SendCounters），支持外部实时监控；MPSC 无锁异步日志系统，分级过滤，IO 线程零阻塞

▸ 内存池 + 平坦数据结构：OrderPool 预分配 4M 定长槽位（无 malloc，O(1) allocate/deallocate），OrderMap 哈希桶链长 > 8 时自动溢出到 std::map 防退化

## 性能

**2026-06-17 真实行情数据回放**

数据源：akshare 腾讯财经，平安银行 / 贵州茅台 / 招商银行等 20 只沪深股票，69,633 条逐笔成交 → 80,217 笔订单（含 ~20% 撤单）。

| 指标 | 数值 |
|:----|:----:|
| Python 全速回放 | **5,904,057 QPS** / 80,217 笔 100% 完成 |
| 单笔延迟（本地回环） | 5 µs |
| Pipeline 吞吐 | 1,390 万 QPS |
| 硬件 | 12th Gen i9-12900HX / Ubuntu 22.04 / Linux 6.8.0 |
| 绑核 | IO+Matching core 6, Send core 7, Client core 5（P-core） |

## 项目结构

```
NebulaX/
├── include/                   # 头文件
│   ├── matching_engine.h          撮合引擎
│   ├── order_book.h               买卖盘 + OrderMap 索引
│   ├── order_pool.h               mmap 共享内存订单池
│   ├── order_map.h                哈希链 > 8 溢出到 std::map
│   ├── protocol.h                 二进制协议（32B 命令 / 48B 响应）
│   ├── tcp_server.h               io_uring 服务端
│   ├── io_uring_poller.h          io_uring 封装（固定缓冲区 + 超时处理）
│   ├── spsc_byte_ring.h           SPSC 无锁字节环形缓冲区
│   ├── wal.h                      WAL 环形 mmap
│   ├── trade_pool.h               成交环形缓冲区（共享内存）
│   ├── logger.h                   MPSC 无锁异步日志
│   ├── metrics.h                  共享内存指标
│   ├── shutdown_guard.h           优雅关闭
│   ├── crash_guard.h              SIGSEGV/SIGABRT handler
│   ├── mpsc_ring.h                MPSC 无锁环形队列
│   └── order.h                    订单定义
├── src/                       # 实现
├── benchmark/                 # 压测
│   └── benchmark_client.cpp
├── scripts/
│   ├── nebulaX_bench.sh           压测 + perf + 火焰图
│   └── l2_replay.py               真实行情回放
├── data/
│   └── l2_replay.csv              80,217 笔（沪深 20 只股票逐笔）
├── docs/
│   ├── BENCHMARK.md
│   └── optimizations/             各阶段优化报告
├── profiling/                 # 火焰图
└── README.md
```

## 架构

![V3 架构图](docs/images/V3架构图.png)

### 数据流

```
Client → [TCP] → io_uring recv → onRecv → MatchingEngine → pushResponses
                                                              ↕ SPSC ring
                                                         Send 线程 → [TCP] → Client
```

### Matching Engine

Price-Time Priority（价格优先 + 时间优先 FIFO），支持部分成交、撤单。`std::map<price, PriceLevel>` 买卖盘 + OrderMap O(1) 撤单索引。

## 优化路线

| 阶段 | 内容 |
|:----|------|
| V1 | 纯文本协议基线（69 万 QPS） |
| Phase 3 | 二进制协议 + 批量收发 |
| Phase 4 | epoll ET reactor + 多连接 |
| Phase 5 rev2 | 双线程 + SPSC byte ring + 快速路径 |
| Phase 6 | 内存池 + 平坦数据结构（4M 定长槽） |
| Phase 7 | io_uring recv + SEND_ZC，recv/send 零拷贝（**1,390 万 QPS**） |
| Phase 8 | WAL + 共享内存 + 崩溃恢复 + 异步日志 + 监控指标 + 心跳检测 |
| Phase 9 | 真实行情回放 + timeout SQE user_data bug 修复（**590 万 QPS，真实数据**） |

经历 9 轮迭代，Pipeline QPS 从 69 万提升至 1,390 万（+20 倍），尾延迟从 270ms 降至 56ms（-96%）。每轮使用 perf + 火焰图定位瓶颈，数据驱动决策。

## Benchmark

```bash
sudo bash ./scripts/nebulaX_bench.sh              # pipeline 模式
sudo bash ./scripts/nebulaX_bench.sh -r           # ping-pong 模式
python3 scripts/l2_replay.py 2250                 # 真实行情回放
```

Pipeline/Pingpong 双模式，RDTSC 计时，perf stat 硬件事件采集。输出 QPS / 延迟分布 / perf 热点 / 火焰图 / IPC / cache-misses / ctx/s。

## 测试

- **单元测试**：33 用例覆盖 SPSC/MPSC/OrderPool/OrderBook/Protocol/OrderMap 溢出
- **集成测试**：build → 下单 → SIGSEGV → 共享内存恢复 → WAL 回放
- **崩溃恢复**：发单后 kill -SEGV，重启后 100% 数据完好
- **真实数据回放**：2026-06-17 沪深 20 只股票逐笔成交，80,217 笔 100% 完成

## 环境

- 构建：CMake + g++11（Ubuntu 22.04）+ liburing 2.4
- 压测：C++17，POSIX sockets，taskset 绑核
- 分析：perf dwarf unwind，FlameGraph
