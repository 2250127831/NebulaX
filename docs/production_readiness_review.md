# NebulaX 生产就绪技术审查报告

**审查日期：** 2026-06-04
**审查范围：** 全部源码（14 个文件）、构建系统（CMakeLists.txt）、脚本、文档
**架构背景：** Client → Gateway → NebulaX（撮合引擎）

---

## 目录

1. [审查范围与方法论](#1-审查范围与方法论)
2. [致命缺陷：无优雅关闭机制（P0）](#2-致命缺陷无优雅关闭机制p0)
3. [线程无 liveness 监控——崩溃/hang 无法感知（P1）](#3-线程无-liveness-监控崩溃hang-无法感知p1)
4. [SPSC ring 满导致 IO 线程忙等——无背压机制（P0）](#4-spsc-ring-满导致-io-线程忙等无背压机制p0)
5. [io_uring CQE 错误无差异化处理（P1）](#5-io_uring-cqe-错误无差异化处理p1)
6. [连接管理：无空闲超时 + 关闭竞态（P1）](#6-连接管理无空闲超时--关闭竞态p1)
7. [内存池全满后无降级策略（P1）](#7-内存池全满后无降级策略p1)
8. [全无监控与度量（P1）](#8-全无监控与度量p1)
9. [缺少结构化日志（P2）](#9-缺少结构化日志p2)
10. [无持久化——重启即丢失全部状态（P2）](#10-无持久化重启即丢失全部状态p2)
11. [单线程 IO + 撮合 = 阻塞点（P2）](#11-单线程-io--撮合--阻塞点p2)
12. [SEND_ZC 路径数据丢失风险（P2）](#12-send_zc-路径数据丢失风险p2)
13. [连接关闭存在竞态条件（P2）](#13-连接关闭存在竞态条件p2)
14. [book 只返回 top-of-book，深度信息不足](#14-book-只返回-top-of-book深度信息不足)
15. [测试覆盖缺口（P2）](#15-测试覆盖缺口p2)
16. [二进制协议缺少保护措施](#16-二进制协议缺少保护措施)
17. [无 DoS 防护与安全基线](#17-无-dos-防护与安全基线)
18. [构建与部署（P3）](#18-构建与部署p3)
19. [Gateway 架构下的修正优先级](#19-gateway-架构下的修正优先级)
20. [总结：问题优先级总表](#20-总结问题优先级总表)
21. [多线程架构扩展审查](#21-多线程架构扩展审查)

---

## 1. 审查范围与方法论

### 架构前提

本系统在真实部署中的位置：

```
Client (交易者)
    ↓ TCP (外部网络)
Gateway (安全接入层)
    ↓ TCP (内部网络)
NebulaX (撮合引擎)
```

Gateway 负责 TLS 终止、认证鉴权、连接管理、协议校验、速率限制。
NebulaX 专注一件事：以最低延迟做订单簿撮合。

因此本审查的权重发生以下转移：

| 由 Gateway 负责（本报告降权） | NebulaX 自身仍需解决（本报告重点） |
|:-----------------------------|:---------------------------------|
| TLS / 信道加密 | 进程生命周期管理（优雅关闭） |
| 客户端认证 / user_id 鉴权 | 内部线程同步与竞态 |
| IP 级速率限制 | 组件的背压与流控 |
| 协议魔数校验 | 资源耗尽保护 |
| 连接级 DoS | 内部可观测性（无 Gateway 排障盲区） |

### 每个问题的结构

```
问题描述 → 生产风险 → 改进方案（含代码或伪代码） → 性能影响分析
```

### 性能设计原则

本报告的所有改进方案遵循以下约束：

1. **热路径（Hot path）零额外 syscall。** IO/Match 线程的每命令路径不可增加任何系统调用（write、open、fsync 等）。
2. **热路径避免原子操作争抢。** 跨线程共享状态使用`memory_order_relaxed`或纯非原子计数器放在独立 cache line 中。
3. **降级路径不计入性能预算。** 背压触发、日志记录、pool 水位检查等非常规路径的代价可以接受，因为它们不发生在稳态快速路径上。
4. **x86-64 优先。** 当前目标为 x86（强内存模型），`release`/`acquire` 的 CPU 开销几乎为零，ARM 等弱内存模型暂不考虑。
5. **每项方案必须评估对 P50 和 P99 的影响。** 不能为了 0.01% 的极端场景在 99.99% 的正常路径上付出代价。

---

## 2. 致命缺陷：无优雅关闭机制（P0）

### 发现的问题

TcpServer::start()（`src/tcp_server.cpp:87`）和发送线程（`src/main.cpp:70`）都是 `while (true)` 无限循环。整个进程唯一的退出方式是 `pkill -9`。

```cpp
// src/main.cpp:70 — 发送线程永远不返回
while (true) {
    uint8_t hdr[48];
    if (ring.pop(hdr, 48) == 0) {
        // ...
    }
    // ...
}
```

### 生产风险

- **数据丢失：** 发送线程正在从 SPSC ring 读数据准备发送时，SIGKILL 让 ring 中最多 1MB 的待发送响应帧全部丢失
- **文件描述符泄漏：** 线程被杀死后 TcpServer 析构函数不会执行，所有 client fd、server fd、eventfd、io_uring fd 全部泄漏
- **状态不一致：** MatchingEngine 中的 OrderBook 可能有未处理完的订单，重启后丢失再交易时会出错
- **容器编排不兼容：** K8s/Docker 依赖 SIGTERM 做优雅停止，SIGTERM 杀掉后系统残留大量 TIME_WAIT 连接
- **Gateway 视角：** Gateway 不知道 NebulaX 是主动关闭还是崩溃，可能触发不必要的重连/重试风暴

### 改进方案

添加信号注册 + 线程安全退出标志 + 有界 drain 超时。

```cpp
// include/shutdown.h
#pragma once
#include <atomic>
#include <csignal>
#include <iostream>

class ShutdownGuard {
    static std::atomic<bool> stopped_;
public:
    static void handleSignal(int sig) {
        stopped_.store(true, std::memory_order_release);
    }
    static bool isStopping() {
        return stopped_.load(std::memory_order_acquire);
    }
    static void install() {
        struct sigaction sa;
        sa.sa_handler = handleSignal;
        sigfillset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT,  &sa, nullptr);
    }
};
```

在 IO 和 Send 线程循环中检查退出标志：

```cpp
// src/main.cpp — 发送线程
while (!ShutdownGuard::isStopping()) {
    // ... 现有发送逻辑
    // 如果 ring 为空且正在停止，退出
    if (ShutdownGuard::isStopping() && ring.free_space() == RING_SIZE)
        break;
}

// src/tcp_server.cpp — TcpServer::start()
while (!ShutdownGuard::isStopping()) {
    int ret = poller_.submit_and_wait();
    // ...
}

// src/main.cpp — 主线程添加 drain 超时
int main() {
    ShutdownGuard::install();
    // ... 创建线程 ...

    // 收到信号后，先等 IO 线程结束（TcpServer 析构时刷所有连接）
    io_thread.join();
    // drain：等 ring 空或超时（给 send 线程机会发完）
    auto deadline = steady_clock::now() + seconds(5);
    while (ring.free_space() < RING_SIZE && steady_clock::now() < deadline) {
        __builtin_ia32_pause();
    }
    send_thread.join();
    return 0;
}
```

---

## 3. 线程无 liveness 监控 —— 崩溃/hang 无法感知（P1）

### 发现的问题

当前两线程都是裸 `while (true)` 循环，没有任何 liveness 检测机制。

```cpp
// src/main.cpp:70 — 发送线程
while (true) {
    uint8_t hdr[48];
    if (ring.pop(hdr, 48) == 0) {
        uint64_t val;
        read(wake_fd, &val, 8);
        continue;
    }
    // ...
}

// src/tcp_server.cpp:87 — IO 线程
while (true) {
    int ret = poller_.submit_and_wait();
    // ...
}
```

### 生产风险

**线程 hang 场景（三个各不相同）：**

| 场景 | 原因 | 表现 |
|:-----|:-----|:-----|
| IO 线程 `pushResponses` 自旋 | Send 线程挂了，ring 永远满 | `while (ring.push == 0)` 无限循环，进程"死而不僵" |
| Send 线程 `send()` 阻塞 | Gateway 侧不消费，send buffer 满（send 线程未设置 non-blocking socket） | Send 线程卡住，ring 迅速填满，IO 线程继而也卡住 |
| IO 线程 `io_uring_submit_and_wait` 不返回 | 内核 bug 或极端条件 | 线程安静地挂起，无超时机制将其唤醒 |

**线程 crash 场景：**

| 场景 | 后果 |
|:-----|:------|
| 任意线程 SIGSEGV / SIGABRT | 整个进程死亡，resting orders 丢失 |
| 没有 signal handler | 无法做最后的紧急 dump |
| 没有 supervisor 感知 | 进程死了但无人拉起，Gateway 侧连接全部超时 |

**关键区别：** crash 是进程死（外部 supervisor 可以重启），hang 是进程不死但也不工作（比 crash 更糟——资源被占用、Gateway 连接保持、监控显示进程存活但业务停摆）。

### 改进方案

**第一层：线程心跳（Thread Heartbeat）**

每线程在循环顶部更新一个共享时间戳。Watchdog（主线程）定期检查。

```cpp
// include/thread_monitor.h
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <csignal>

enum class ThreadId : uint8_t {
    IO_MATCH = 0,
    SEND     = 1,
    COUNT
};

struct alignas(64) ThreadHeartbeat {
    std::atomic<uint64_t> last_tick_ns{0};
    std::atomic<bool> exited{false};
};

class ThreadMonitor {
public:
    static ThreadHeartbeat heartbeats[static_cast<int>(ThreadId::COUNT)];

    static void tick(ThreadId id) {
        heartbeats[static_cast<int>(id)].last_tick_ns.store(
            getMonotonicCoarseNs(), std::memory_order_release);
    }

    // 检查所有线程是否在最近 timeout_ms 内 tick 过
    // 返回 hang 的线程 ID，无 hang 返回 -1
    static int checkHang(uint64_t timeout_ms) {
        uint64_t now = getMonotonicCoarseNs();
        for (int i = 0; i < static_cast<int>(ThreadId::COUNT); ++i) {
            auto& hb = heartbeats[i];
            if (hb.exited.load()) continue;
            uint64_t last = hb.last_tick_ns.load(std::memory_order_acquire);
            if (last == 0) continue;
            uint64_t elapsed_ms = (now - last) / 1000000;
            if (elapsed_ms > timeout_ms) return i;
        }
        return -1;
    }
};
```

在循环中插桩：

```cpp
// IO 线程
TcpServer server(port, engine, ring, wake_fd);
while (!ShutdownGuard::isStopping()) {
    ThreadMonitor::tick(ThreadId::IO_MATCH);
    int ret = poller_.submit_and_wait();
    // ...
}

// Send 线程
while (!ShutdownGuard::isStopping()) {
    ThreadMonitor::tick(ThreadId::SEND);
    // ... 现有发送逻辑 ...
}
```

**第二层：Watchdog 线程 + 紧急恢复**

主线程在创建两个工作线程后充当 watchdog，定期检查心跳，发现 hang 后紧急 dump 并触发进程退出。

```cpp
// src/main.cpp — watchdog 循环
ShutdownGuard::install();
// ... 创建 io_thread, send_thread ...

// 主线程充当 watchdog
while (!ShutdownGuard::isStopping()) {
    int hung = ThreadMonitor::checkHang(3000);  // 3s 无 tick = hang
    if (hung >= 0) {
        LOG_FATAL("Thread %d hung, initiating emergency dump", hung);
        emergencyDumpState(engine);
        kill(getpid(), SIGTERM);
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

io_thread.join();
send_thread.join();
```

**第三层：Crash signal handler（最后防线）**

```cpp
// 在 main 函数注册——signal handler 只做 async-signal-safe 操作
void crashHandler(int sig) {
    const char* msg = "FATAL: process crashed, check /tmp/nebulaX_recovery\n";
    write(STDERR_FILENO, msg, strlen(msg));
    // 恢复默认 handler 并重新触发，产生 core dump
    signal(sig, SIG_DFL);
    raise(sig);
}

// main() 中
signal(SIGSEGV, crashHandler);
signal(SIGABRT, crashHandler);
signal(SIGBUS,  crashHandler);
```

**第四层：Non-blocking socket 修复——预防 send 线程 hang**

当前 Send 线程的 `send()` 未设置 non-blocking，Gateway 侧 TCP 窗口满时会阻塞，使事件链锁死：

```cpp
// Send 线程中设置 non-blocking
// 方案 A：在 ConnContext 创建时设置（需要跨线程传递 fd）
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);

// 方案 B：send 时使用 MSG_DONTWAIT（影响更局部）
ssize_t r = send(fd, ptr, chunk, MSG_NOSIGNAL | MSG_DONTWAIT);

// 配合 EAGAIN 处理
if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    g_metrics.send_buffer_full++;
    // 退避等待，不硬生生阻塞
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    continue;
}
```

**第五层：进程级 supervisor**

线程 crash 后共享内存状态（MatchingEngine 中的 OrderPool、IoUringPoller 的固定缓冲区）的一致性不可保证。**可靠恢复只能靠进程级重启：**

```
NebulaX 进程 crash/hang
    → signal handler / watchdog 察觉
    → 紧急写恢复文件到磁盘（async-signal-safe write）
    → 进程退出
    → systemd / K8s / supervisor 检测到退出
    → 重启进程
    → 读取恢复文件重建 OrderBook
    → Gateway 重连，恢复交易
```

### 优先级说明

标为 P1 而非 P0：心跳+watchdog 的价值需要配合外部 supervisor 才能完整发挥。原型阶段可接受手动重启，但上 K8s 时必须配上——否则进程 hang 时 Pod 显示 Running 但业务完全停摆，K8s 不会自动拉起。

### 预期工作量

| 子项 | 预估 |
|:-----|:----:|
| 心跳计数器 + 循环插桩 | 1 天 |
| Watchdog 检测 + 紧急 dump | 1 天 |
| Crash signal handler | 0.5 天 |
| Non-blocking send 修复 | 0.5 天 |
| 恢复文件格式 + 加载逻辑 | 2 天 |
| **合计** | **约 5 天** |

### 性能影响分析

| 子项 | 热路径代价 | 分析 |
|:-----|:----------|:------|
| 心跳 tick | ~0（x86-64） | `relaxed` store 到自有 cache line（64 字节对齐，不与其他线程共享）。x86 上 release/relaxed 无差别，都是普通 mov。Watchdog 每秒读一次，产生一次 RFO 事件(~50 cycles/秒)——可忽略 |
| Non-blocking send | 负收益 | 原本的 blocking send 在 TCP buffer 满时阻塞数毫秒，`MSG_DONTWAIT` 将阻塞转为 EAGAIN + `pause`，实际降低了 P99 |
| crash signal handler | 不触发 | 只在 segfault/abort 时执行一次，不在任何正常路径上 |
| 恢复文件加载 | 仅启动时 | 进程初始化时回放一次，不影响运行时性能 |

**Watchdog 实现注意：** 不要在 watchdog 中使用 `std::this_thread::sleep_for`（涉及 syscall），改用 `io_uring_wait_cqe_timeout` 或 `ppoll` 做定时等待，避免 watchdog 自身产生上下文切换开销。但 watchdog 本身不在热路径上，所以这点影响不大。

---

## 4. SPSC ring 满导致 IO 线程忙等——无背压机制（P0）

### 发现的问题

`pushResponses`（`src/tcp_server.cpp:225-242`）中，当 ring 满时 IO 线程陷入 **busy-wait 循环**：

```cpp
// src/tcp_server.cpp:225-228
while (ring_.push(&header, sizeof(BinaryResponse)) == 0) {
    notifySendThread();
    __builtin_ia32_pause();
}
```

同样的模式也出现在 `closeConnection`（`tcp_server.cpp:177`）。

### 生产风险

- Send 线程如果因为上下文切换或大包发送被短时阻塞，IO 线程就忙等——该连接的所有响应堆积，**其他连接的撮合也停顿**（单 IO 线程模型）
- 测试数据显示 1MB ring 在 Phase 6 12.2M QPS 时 P999 高达 1,326ms，主要原因是 IO 线程等 ring 空间
- Gateway 模式下问题放大：Gateway 攒批发送的突发量可能远超单个客户端，一次大 batch 进 IO 线程，生成大量响应帧，1MB ring 在微秒级就被填满
- 极端情况下 Send 线程崩溃，IO 线程永远卡在这个循环里，无法处理任何新请求，整个引擎挂死

### 改进方案

**方案 A：预检空间 + 有界重试 + 自适应降级**

```cpp
// tcp_server.h
static constexpr int SPIN_LIMIT = 10000;  // 约 100µs @ 3GHz PAUSE loop

// tcp_server.cpp
void TcpServer::pushResponses(int fd, const std::vector<BinaryResponse>& buf) {
    size_t count = buf.size();
    if (count == 0) return;
    size_t bytes = count * sizeof(BinaryResponse);

    // 快速路径：小 batch 且 ring 空闲，直接 send 绕过队列
    // ...（原有逻辑不变）...

    // 预判剩余空间：header + 全部数据必须能完整塞入
    // 若空间不足且自旋超过上限，直接降级，不动 ring
    size_t need_total = sizeof(BinaryResponse) + bytes;  // header + 数据
    int retries = 0;
    while (ring_.free_space() < need_total) {
        notifySendThread();
        if (++retries > SPIN_LIMIT) {
            // 降级：直接 send，不动 ring
            g_metrics.ring_full_events++;
            fallbackDirectSend(fd, buf);
            return;
        }
        __builtin_ia32_pause();
    }

    // 空间充足，推 Header（此时必然成功）
    ring_.push(&header, sizeof(BinaryResponse));

    // 推数据帧（此时必然成功）
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(buf.data());
    size_t remaining = bytes;
    size_t off = 0;
    while (remaining > 0) {
        size_t n = ring_.push(ptr + off, remaining);
        // 预判已保证空间，此处 n > 0 恒成立
        off += n;
        remaining -= n;
    }

    notifySendThread();
}
```

**关键改进：先检查 `free_space()` 再决定是否入队，header 和数据要么都进要么都不进。** 避免了原方案中 header 已入队但数据推不进去导致 Send 线程读到空头的竞态问题。`rollbackRing()` 不再需要。
    }
    notifySendThread();
}
```

**方案 B：Ring 扩容（立即见效的工程措施）**

Ring 从 1MB 扩至 8MB。Phase 6 实测 1MB→8MB 后 P999 从 1,326ms 降至 774ms（-42%）。8MB 连续内存在现代服务端上是可接受的（相对于性能收益）。

```cpp
// include/tcp_server.h — 修改
constexpr size_t RING_SIZE = 8388608;  // 8 MB
```

**方案 C：Ring 利用率监控**

```cpp
// 每次 push 后采样
size_t used = RING_SIZE - ring_.free_space();
double pct = used * 100.0 / RING_SIZE;
if (pct > 80) {
    g_metrics.ring_peak_util = std::max(g_metrics.ring_peak_util, pct);
}
```

### 性能影响分析

**快速路径（空间充足）代价：**

```cpp
int retries = 0;
while (ring_.free_space() < need_total) {  // 空间够 → 不进循环体
    // 循环体只在空间不足时执行
}
ring_.push(&header);  // 必然成功
ring_.push(&data);    // 必然成功
```

稳态下 `free_space()` 一次调用（两次 atomic load：`acquire` + `relaxed`，x86 上约几 cycle）。之后 push 成功，不进入循环体。与修改前相比，多了一次 `free_space()` 检查，少了 `push()` 返回 0 时要处理的分支——两者开销相当。

**慢速路径（拥塞）代价：** 循环体内 `notifySendThread()`（eventfd write ~1µs）+ `__builtin_ia32_pause()`（~10 cycles）。`SPIN_LIMIT` 上限保护，超限后降级直接发送，不再动 ring。拥塞本身已是异常状态，此代价可接受。

**Ring 利用率采样代价：** 每批响应推完后调用 `ring_.free_space()`（两次 `atomic.load(acquire/relaxed)` ≈ 0）。只在高水位采样。整体热路径影响：**零新增指令**（快速路径上与修改前完全一致）。

---

## 5. io_uring CQE 错误无差异化处理（P1）

### 发现的问题

`process_cqes`（`include/io_uring_poller.h:98-118`）中，所有的 `cqe->res` 不做区分地交给 on_recv 回调：

```cpp
// include/io_uring_poller.h:109-114
if (fd == server_fd) {
    if (res >= 0)
        on_accept(res);
} else {
    on_recv(fd, res);
}
```

`onRecv` 只判断 `<= 0`（`src/tcp_server.cpp:138`）：

```cpp
if (bytes_read <= 0) {
    closeConnection(conn);
    return;
}
```

### 生产风险

- io_uring_recv 返回 `-EAGAIN` 表示内核缓冲区暂时无数据，这是合法瞬态——但 `closeConnection` 会直接断开连接
- `-EINTR` 虽然在 `submit_and_wait` 层被忽略，但 CQE 层的错误不会被重试
- Gateway 与 NebulaX 之间的连接出现瞬态问题时，NebulaX 会直接关闭连接而非重试

### 改进方案

```cpp
// include/io_uring_poller.h
void process_cqes(int server_fd,
                  const std::function<void(int)>& on_accept,
                  const std::function<void(int, int)>& on_recv,
                  const std::function<void(int, int)>& on_recv_error)  // 新增
{
    struct io_uring_cqe* cqe;
    unsigned head;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        int fd = static_cast<int>(
            reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));
        int res = cqe->res;

        if (fd == server_fd) {
            if (res >= 0)
                on_accept(res);
            // accept -EAGAIN 表示无新连接，正常
            else if (res != -EAGAIN)
                logError("accept failed", res);
        } else {
            if (res > 0) {
                on_recv(fd, res);
            } else if (res == -EAGAIN) {
                // 瞬态空缓冲区——重新提交 recv
                // 不触发 on_recv，直接 re-arm
                // on_recv_error(fd, res);
            } else if (res == -ECONNRESET || res == -EPIPE) {
                on_recv(fd, 0);  // 正常断开
            } else {
                logError("io_uring recv error", res, fd);
                on_recv(fd, 0);
            }
        }
        io_uring_cqe_seen(&ring_, cqe);
    }
}
```

---

## 6. 连接管理：无空闲超时 + 关闭竞态（P1）

### 发现的问题

`ConnContext`（`include/tcp_server.h:15-31`）只有读缓冲区状态管理，没有任何超时或心跳机制。

```cpp
struct ConnContext {
    int fd;
    char* read_buf = nullptr;
    uint32_t buf_idx = UINT32_MAX;
    size_t pending = 0;
    size_t consumed = 0;
    // 缺失：last_activity, closing 标志
};
```

### 生产风险

- Gateway 到 NebulaX 的连接如果空闲但未关闭（比如 Gateway 工作线程池模式），buffer 被长期占用
- 最大 64 个固定缓冲区，**64 条空闲连接可以把服务器耗尽**，新连接无法接入
- Gateway 异常断开（进程崩溃），NebulaX 侧的 TCP 连接不被关闭（没有 FIN），永远不会知道连接已死

### 改进方案

在 ConnContext 添加最后活动时间与定时扫描：

```cpp
struct ConnContext {
    int fd;
    char* read_buf = nullptr;
    uint32_t buf_idx = UINT32_MAX;
    size_t pending = 0;
    size_t consumed = 0;
    uint64_t last_recv_ns = 0;   // 最新一次 recv 的 CLOCK_MONOTONIC
    bool closing = false;         // 正在关闭中
};

// TcpServer 添加 idle check 成员
// 在 start() 循环中每 N 次 CQE 处理检查一次
void TcpServer::checkIdleConnections() {
    uint64_t now = getMonotonicCoarseNs();
    for (auto& [fd, conn] : conns_) {
        if (now - conn->last_recv_ns > IDLE_TIMEOUT_NS) {
            LOG_WARN("closing idle connection: fd=%d idle_ms=%llu",
                     fd, (now - conn->last_recv_ns) / 1000000);
            closeConnection(conn);
        }
    }
}
```

---
（正文结束，接续完整报告）

---

## 7. 内存池全满后无降级策略（P1）

### 发现的问题

`OrderPool`（`include/order_pool.h`）固定 4M 条目，满时返回 `nullptr`。`MatchingEngine` 返回 `POOL_FULL` 错误（`src/matching_engine.cpp:47-50`）。

```cpp
// matching_engine.cpp:47-50
if (!order_book_.addOrder(order)) {
    auto& rsp = out.emplace_back();
    rsp.type = RSP_ERROR;
    rsp.data.error.code = static_cast<uint16_t>(ErrorCode::POOL_FULL);
}
```

### 生产风险

- 4M 订单（≈64MB）在极端场景下可能耗尽：高频交易、Gateway 重试风暴
- **无主动清理机制**：resting order 永远存在直到被成交或撤销。一个 bug 导致大量无法成交的订单可耗尽 pool
- Gateway 可能带重试语义，收到 `POOL_FULL` 后重试，加剧耗尽
- OrderPool 和 OrderMap 使用两个独立容量常量（虽然值相同但无编译期校验）

### 改进方案

**方案 A：水位监控 + 渐进降级**

```cpp
// OrderBook 添加压力级别
enum class PoolPressure { NORMAL, WARNING, CRITICAL };

PoolPressure OrderBook::getPoolPressure() const {
    double usage = pool_.size() * 100.0 / pool_.capacity();
    if (usage > 95) return PoolPressure::CRITICAL;
    if (usage > 80) return PoolPressure::WARNING;
    return PoolPressure::NORMAL;
}
```

| 水位 | 动作 |
|:-----|:-----|
| < 80% | 正常处理 |
| 80-95% | 记录告警日志 + 上报指标 + 限制 NEW 订单 |
| > 95% | 拒绝 NEW，允许 CANCEL 和 BOOK |

**方案 B：Pool 容量可配**

```cpp
// include/order_book.h
explicit OrderBook(size_t pool_capacity /* 从配置读取，默认 4M */);
```

---

## 8. 全无监控与度量（P1）

### 发现的问题

生产可观测性指标完全为零。

- 唯一输出是 `write(STDERR_FILENO, ...)` 的字符串（启动错误）
- `perf stat` 只能离线使用，不能接入生产
- `lensx_markers.cpp` 中的 eBPF uprobe 锚点是空函数，且需要内核 eBPF 基础设施

### 生产风险

- 无法回答最基本的运维问题："现在 QPS 多少？延迟 P99 正常吗？"
- 无法做容量规划：不知道 pool 利用率趋势，不知道连接数峰值
- Gateway 看到的延迟包含网络，**只有 NebulaX 自身指标才能定位内部瓶颈**
- 问题排查只能 SSH 进机器跑 perf，不符合 SRE 最佳实践

### 改进方案

添加一个轻量级、无锁的指标收集层，通过共享内存暴露给外部采集器。

**无锁计数器（每个线程独占一个 cache line）：**

```cpp
// include/metrics.h
#pragma once
#include <cstdint>

// ⚠ 所有计数器都是非原子的 plain uint64_t。
// 每个线程只写自己专属的 cache line（64 字节对齐），外部程序通过共享内存只读读取。
// 外部读到略微 stale 的值是可接受的（微秒级差异不影响监控决策）。
// 禁止使用 std::atomic —— 热路径上的 atomic inc 会产生 L1 cache 锁，拖慢 ~30 cycles/次。
struct alignas(64) MetricsCacheLine {
    uint64_t commands_received;     // +1 per 命令
    uint64_t commands_new;          // +1 per CMD_NEW
    uint64_t commands_cancel;       // +1 per CMD_CANCEL
    uint64_t commands_book;         // +1 per CMD_BOOK
    uint64_t orders_placed;         // +1 per 成功 resting
    uint64_t orders_cancelled;      // +1 per 成功撤单
    uint64_t trades_executed;       // +1 per TRADE
    uint64_t total_trade_quantity;  // +trade_qty
    uint64_t bytes_sent;            // +sent_bytes
    uint64_t bytes_received;        // +recv_bytes
    uint64_t errors_total;          // +1 per 错误
    uint64_t errors_pool_full;      // +1 per POOL_FULL
    uint64_t ring_full_events;      // +1 per ring 满降级
    uint64_t ring_peak_util;        // max(ring_utilization %)
    uint64_t connections_current;   // 当前连接数（IO 线程 set/clear）
    uint64_t connections_accepted;  // 累计 accept 数
};

// IO+Matching 线程和 Send 线程各持一个——每个独占一条 cache line
// 内存布局：[g_io_metrics: 64B] [g_send_metrics: 64B]
extern MetricsCacheLine g_io_metrics;
extern MetricsCacheLine g_send_metrics;

// 热路径上的递增——代价是一条 `inc [addr]` 指令（~1 cycle，L1 命中）
#define METRIC_INC(field)  do { ++g_io_metrics.field; } while(0)
```

**数据暴露方式（推荐：共享内存 mmap）：**

```cpp
// 初始化时（main 函数启动阶段）
const char* shm_name = "/nebulaX_metrics";
int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0444);
ftruncate(shm_fd, sizeof(g_io_metrics) + sizeof(g_send_metrics));
// mmap 两个 cache line，将 MetricsCacheLine 映射到共享内存
void* addr = mmap(nullptr, ..., MAP_SHARED, shm_fd, 0);
// 将 g_io_metrics 和 g_send_metrics 放置到共享内存区域
// 外部采集器（Prometheus node_exporter textfile collector 或 sidecar）
// 通过 shm_open + mmap 读取，每秒采样一次
```

| 暴露方式 | 复杂度 | 热路径影响 | 推荐度 |
|:---------|:------:|:----------:|:------:|
| 共享内存 mmap | 低 | 无（外部进程读取，不中断工作线程）| ★ |
| Unix domain socket | 中 | 有（poll + write）| 仅 debug |
| /metrics HTTP endpoint | 高 | 有（需 http 解析）| 不推荐 |

**最小告警阈值：**

| 指标 | Warning | Critical |
|:-----|:-------|:---------|
| ring 利用率 | > 80% | > 95% |
| pool 利用率 | > 70% | > 90% |
| 错误率 (/s) | > 100 | > 1000 |
| 连接数 | > 50 | — |
| ctx/s（perf 监控） | — | > 10000 |

### 性能影响分析

| 子项 | 热路径代价 | 分析 |
|:-----|:----------|:------|
| `METRIC_INC`（纯 `inc` 指令）| ~1 cycle | L1 cache 命中，非原子指令，单线程独占的 cache line，无锁总线事务 |
| 共享内存映射 | 0 | mmap 在初始化时完成，运行时无任何额外指令 |
| 外部采集器读 shm | ~0 | 外部进程独立 mmap。Intel CPU 上 reader 不产生 RFO 请求，不污染 writer 的 L1 cache |

**为什么不能用 `std::atomic`：** 实测 `++counter`（普通 inc）vs `counter.fetch_add(1, relaxed)`（lock add）在当前代 Intel CPU 上差距约 20-30 cycles/次。对于每秒千万级的命令路径，仅 `commands_received` 一个计数器就损失 ~200M cycles/s ≈ 70ms CPU 时间/P-core。而普通 `inc` 独占 cache line 时约 1 cycle，差了两个数量级。

---

## 9. 缺少结构化日志（P2）

### 发现的问题

```cpp
write(STDOUT_FILENO, msg, sizeof(msg) - 1);   // tcp_server.cpp:52
write(STDERR_FILENO, "ERROR: bind() failed\n", 21);  // tcp_server.cpp:32
```

使用 `write()` 写纯文本字符串。无时间戳、无日志级别、无上下文。多线程输出可能交错。

### 改进方案

添加极简的环形日志缓冲区。延迟关键路径只写内存 buffer，日志线程异步消费写入文件。

```cpp
// include/logger.h
#pragma once
#include <cstdint>
#include <cstdarg>

enum LogLevel : uint8_t {
    LOG_FATAL = 0,
    LOG_ERROR = 1,
    LOG_WARN  = 2,
    LOG_INFO  = 3,
    LOG_DEBUG = 4
};

void logInit(const char* path, LogLevel level);
void logWrite(LogLevel level, const char* file, int line,
              const char* fmt, ...);

#define LOG_FATAL(fmt, ...) \
    logWrite(LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    logWrite(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  \
    logWrite(LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
```

输出格式示例：

```
2026-06-04T10:23:45.123456+08:00  WARN  [IO]  ring_utilization=85%  free=157KB  qps=13400000
2026-06-04T10:23:46.789012+08:00  ERROR [IO]  accept failed: res=-24 (EMFILE)  fd_count=64
```

**性能影响分析：**

| 场景 | 热路径代价 | 分析 |
|:-----|:----------|:------|
| `LOG_INFO`/`LOG_DEBUG` 在热路径上 | **禁止使用** | 任何格式化 + ring push 在千万级 QPS 下都会产生可测量的延迟。info/debug 日志只允许在启动、关闭、配置变更等非关键路径上使用 |
| `LOG_WARN`/`LOG_ERROR` 在热路径上 | 可接受 | 这些路径只在异常条件触发（`ring_full_events`、`pool_high_watermark`），正常运行时每秒触发 0 次，不在稳态性能路径上 |
| ring push 到日志缓冲区 | ~memcpy(200B) ≈ 2ns | 代价相当于多拷贝一个响应帧。日志缓冲区用独立的 SPSC ring（非共享 ring），不影响主数据路径 |
| 日志线程 fsync | 0 | 日志线程运行在独立的、非绑核的线程上，使用 `writev` 批量写文件，`fsync` 每 ~100ms 一次。与 IO/Match/Send 线程无锁竞争 |

**实现约束：** 日志 ring buffer 必须用独立的 `SPSCByteRing` 实例（大小 256KB 足够），不可共享主数据路径的 8MB ring。`LOG_*` 宏中必须先调用 `ShutdownGuard::isStopping()` 或 `__builtin_expect` 做分支预测提示，确保快速路径不会因日志代码的指令缓存污染受影响。

```cpp
// 快速路径上的日志使用 __builtin_expect 引导分支预测
#define LOG_WARN_ONCE(fmt, ...) do { \
    static bool __logged = false; \
    if (__builtin_expect(!__logged, 0)) { \
        logWrite(LOG_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
        __logged = true; \
    } \
} while(0)

---

## 10. 无持久化——重启即丢失全部状态（P2）

### 发现的问题

所有状态（OrderBook、OrderPool、OrderMap、order_id 序列、sequence）全部在内存中，无任何写盘操作。

### 生产风险

- 进程崩溃 → 所有 resting orders 丢失
- 已成交的 TRADE 记录全部消失
- 重启后 `next_order_id_` 从 1 开始，可能产生 ID 冲突
- 做灾备恢复时没有可重放的状态

### 改进方案

**Write-Ahead Log（WAL）+ 异步 checkpoint。** 注意：不可以做每次操作都 fsync 的同步写盘（延迟不可接受）。

```cpp
// include/journal.h
#pragma once
#include <cstdint>

// WAL 记录类型
enum JournalOp : uint8_t {
    JOP_NEW    = 1,  // 新订单
    JOP_CANCEL = 2,  // 撤单
    JOP_TRADE  = 3,  // 成交（可选记录）
};

struct JournalEntry {
    uint8_t  op;
    uint8_t  _pad[7];
    uint64_t timestamp_ns;
    // op-specific payload...
};

class Journal {
    int fd_;
    uint64_t offset_ = 0;
    char* mapped_ = nullptr;  // mmap
    static constexpr size_t FILE_SIZE = 64UL * 1024 * 1024;  // 64MB 循环

public:
    bool init(const char* path);
    void append(const JournalEntry& entry);
    void sync();  // 每 10ms 或每 1000 条调用一次
    void replay(OrderBook& book);
};
```

**恢复流程：**

1. 启动时扫描 WAL 文件
2. 回放所有 `JOP_NEW` 和 `JOP_CANCEL` 重建订单簿
3. 跳过已被成交或撤销的订单
4. `next_order_id_` = max(order_id) + 1

**性能影响分析：**

| 子项 | 热路径代价 | 分析 |
|:-----|:----------|:------|
| `Journal::append()` | 1 次 `memcpy` (32-64 字节) ≈ 0.5ns | 数据写 mmap 内存，等价于一次普通赋值。`offset_` 递增在寄存器完成 |
| `Journal::sync()` | 0（不在热路径）| 在独立定时器线程或 IO 线程的空闲 tick 中执行（每 10ms 或每 1000 条），不影响快速路径 |
| WAL 文件满后回绕 | 0（不在热路径）| 回绕时的 `ftruncate`+`munmap`+`mmap` 在 WAL 线程中同步执行，IO 线程不被阻塞 |

**关键设计约束：**

1. **`append()` 必须与 IO 线程在同一线程**（当前 IO+Matching 线程）。不需要加锁。WAL 写指针是单线程独占的 plain `uint64_t`，非 atomic。
2. **`sync()` 必须在独立的定时器**（`io_uring_prep_timeout` 或 `timerfd`），不可在 `append()` 中同步 fsync。否则每命令延迟多 1-10ms（取决于 fsync 返回时间），QPS 从 13.9M 降至 ~100K。
3. **如果业务容忍宕机丢数秒状态**（大部分交易系统可以接受），WAL 可以完全不做——依赖 Gateway 重连后的状态同步。在这种情况下，SIGTERM handler 中的一次全量 sync 就足够了。

**权衡：** 异步 WAL 在正常运行时对性能的影响约 0.5-1%（纯 memcpy），可以忽略不计。但如果要求"每条命令持久化后才返回 ACK"，延迟会从 5µs 暴涨到 1-10ms（取决于 fsync 延迟），QPS 暴跌至 < 10 万。NebulaX 的架构设计目标是低延迟，**不应做同步持久化**。

---

## 11. 单线程 IO + 撮合 = 阻塞点（P2）

### 发现的问题

`TcpServer::start()` 在同一个线程中处理 accept → recv → parse → match → push 全部工作。FlameGraph 显示撮合逻辑占 CPU 的 47.6%，且随订单量增长占比继续上升。

### 生产风险

- 某个命令的处理时间异常（如 BOOK 返回大量数据），后续所有命令都被阻塞
- IO 线程的 CPU 会最先饱和，而 Send 线程可能空闲
- 无法利用多核扩展

### 改进方案

将当前的双线程架构（IO+Match / Send）升级为三线程架构：

```
Client → Gateway
            │
            ▼
┌──────────────────────┐
│  IO 线程 (core 6)     │  io_uring recv → parse → 推 cmd ring
│  ─────────────        │
│  io_uring CQE         │
│  → onRecv             │
│  → parse command      │
│  → push to CmdRing    │
│  → write(eventfd)     │
└──────────┬───────────┘
           │ cmd ring (SPSC)
           ▼
┌──────────────────────┐
│ Match 线程 (core 5)   │  纯撮合，不碰任何 IO
│  ─────────────        │
│  wait(eventfd)        │
│  → pop from CmdRing   │
│  → MatchingEngine     │
│  → push to RespRing   │
│  → write(eventfd)     │
└──────────┬───────────┘
           │ resp ring (SPSC, 8MB)
           ▼
┌──────────────────────┐
│ Send 线程 (core 7)    │  io_uring SEND_ZC / plain send
│  ─────────────        │
│  wait(eventfd)        │
│  → pop from RespRing  │
│  → send/SEND_ZC      │
└──────────────────────┘
```

**文件变更：**

| 文件 | 变更 |
|:-----|:------|
| `src/main.cpp` | 三线程创建，新增 CmdRing |
| `include/spsc_cmd_ring.h` | 新增——吞吐优先的 SPSC 命令队列 |
| `src/match_thread.cpp` | 新增——撮合线程循环 |
| `include/tcp_server.h` | 去掉引擎引用，只保留 IO |
| `include/matching_engine.h` | 不变 |

**注意：** 这是一个较大重构（预估 2 周），建议先完成 P0/P1 项目再做。

---

## 12. SEND_ZC 路径数据丢失风险（P2）

### 发现的问题

`main.cpp:94-103` 中 SEND_ZC 路径的错误处理：

```cpp
ssize_t r = ring.send_zc_all(fd, need, MSG_NOSIGNAL);
if (r == -EOPNOTSUPP || r == -ENOSYS)
    zc_ok = false;           // 降级
else if (r < 0)
    continue;                // ← 数据已从 ring 释放，客户端永远收不到
else
    continue;
```

`send_zc_all` 内部（`spsc_byte_ring.h:62-119`）在错误路径中会 `read_release(remaining)` 释放剩余数据，而 caller 的 `r < 0` 分支直接 continue 取下一条。**这批响应帧从 ring 中被丢弃，客户端永远不会收到。**

### 改进方案

```cpp
// main.cpp send 线程 SEND_ZC 路径
if (zc_ok && need >= 4096) {
    ssize_t r = ring.send_zc_all(fd, need, MSG_NOSIGNAL);
    if (r == -EOPNOTSUPP || r == -ENOSYS) {
        zc_ok = false;
        // 降级后无法重试（数据已 release），必须关闭连接
        LOG_WARN("SEND_ZC not supported, falling back, closing fd=%d", fd);
        sendCloseSignal(fd);
        continue;
    } else if (r < 0) {
        LOG_ERROR("SEND_ZC failed: fd=%d err=%zd", fd, r);
        sendCloseSignal(fd);
        continue;
    } else {
        continue;
    }
}
```

**改进 `send_zc_all` 设计：** SEND_ZC 失败时不应在函数内 release 数据，而是返回部分发送状态让 caller 决定。但对于低延迟系统而言，更好的策略是"快速失败 + 关闭连接"，让客户端（Gateway）重连恢复。

---

## 13. 连接关闭存在竞态条件（P2）

### 发现的问题

`closeConnection`（`src/tcp_server.cpp:168-187`）：

```cpp
void TcpServer::closeConnection(ConnContext* conn) {
    int fd = conn->fd;
    // 先通知 Send 线程关 fd
    // ...
    // 再清理 IO 线程资源
    poller_.free_buffer(conn->buf_idx);
    conns_.erase(fd);
    delete conn;
}
```

时间窗口：Send 线程执行 `close(fd)` 的同时，IO 线程还持有 `conns_[fd]`。如果此时一个新的连接到达并使用同一个 fd 号（Linux 分配 fd 是递增的，但达到上限后会回绕），`conns_[fd]` 指向已释放的 ConnContext*。

### 改进方案

添加 `ConnContext::closing` 标志 + 确保 IO 线程在新 conn 到来时不会撞到 stale 条目：

```cpp
// 方案一：标记-延迟清理
void TcpServer::closeConnection(ConnContext* conn) {
    if (conn->closing) return;       // 防止重入
    conn->closing = true;
    int fd = conn->fd;

    // 推 RSP_CLOSE
    BinaryResponse frame;
    frame.type = RSP_CLOSE;
    frame.data.header.client_fd = fd;
    // ... push to ring ...

    // 不从 conns_ 删除，只标记 closing
    // Send 线程 close 后通过另一个机制通知 IO 线程清理
}

// Send 线程 close 完后：
void TcpServer::onSendCloseComplete(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    auto* conn = it->second;
    if (!conn->closing) return;  // safety

    poller_.free_buffer(conn->buf_idx);
    conns_.erase(fd);
    delete conn;
}

// 方案二（更简单）：onAccept 中检查并覆盖
void TcpServer::onAccept(int client_fd) {
    auto it = conns_.find(client_fd);
    if (it != conns_.end()) {
        // fd 被重用，旧 conn 已被 Send 线程 close
        // 但 ConnContext* 可能还持有资源
        // 等待清理完成或直接覆盖
    }
    // ...
}
```

---

## 14. book 只返回 top-of-book，深度信息不足

### 发现的问题

`CMD_BOOK` 只返回 TopOfBook（最高买价 + 最低卖价），且 `book_volume` 只读 head_idx 的 `remaining_qty` 而非该价位的总 depth：

```cpp
// order_book.cpp:128-129
const Order* o = pool_.at(level.head_idx);
tob.bid_volume = o->remaining_qty;  // ← 只读了第一个订单的量
```

### 改进方案

将该价位的所有订单量累加：

```cpp
TopOfBook OrderBook::getTopOfBook() const {
    TopOfBook tob;
    if (!bids_.empty()) {
        const PriceLevel& level = bids_.begin()->second;
        tob.bid_price = bids_.begin()->first;
        uint32_t idx = level.head_idx;
        while (idx != UINT32_MAX) {
            tob.bid_volume += pool_.at(idx)->remaining_qty;
            idx = pool_.at(idx)->next_idx;
        }
    }
    // ... asks_ 同理 ...
    return tob;
}
```

---

## 15. 测试覆盖缺口（P2）

### 发现的问题

现存 5 个测试用例（`test/test_correctness.cpp`）覆盖基本面，但存在严重缺口：

1. 每个测试创建真实 TCP 连接 + 子进程（`sleep 0.5` × 5 次），测试慢且不稳定
2. **无单元测试**——OrderPool 边界、OrderMap 哈希碰撞、MatchingEngine 极端场景
3. **无错误注入**——不测试 Send 线程挂掉时 IO 线程的行为、ring 满时的降级路径
4. **无一致性校验**——高并发撮合出入场总成交量是否守恒

### 改进方案

**第一层：纯单元测试（不启动进程、不建 TCP 连接）**

```cpp
// test/test_order_pool.cpp
#include "order_pool.h"
#include <cassert>

void test_pool_exhaustion() {
    OrderPool pool(1024);
    std::vector<Order*> allocd;
    for (int i = 0; i < 1024; ++i) {
        auto* o = pool.allocate();
        assert(o != nullptr);
        allocd.push_back(o);
    }
    assert(pool.allocate() == nullptr);  // 满
    // 释放后重新分配
    pool.deallocate(allocd[0]);
    assert(pool.allocate() != nullptr);
    printf("PASS: pool_exhaustion\n");
}
```

```cpp
// test/test_matching_engine.cpp
void test_self_trade_prevention() {
    MatchingEngine engine;
    std::vector<BinaryResponse> out;
    // 同一个 user_id 同时有买和卖
    engine.processNewOrder(Side::SELL, 10000, 10, 1001, out);
    engine.processNewOrder(Side::BUY,  10000, 10, 1001, out);  // 不应自成交
    // 验证没有产生 TRADE
    int trades = 0;
    for (auto& r : out) if (r.type == RSP_TRADE) trades++;
    assert(trades == 0);
}
```

**第二层：确定性合成测试**

录制一份 1M 命令序列的 trace，跑两遍检查输出是否完全一致。验证确定性（deterministic）撮合行为。

**第三层：一致性校验器**

```cpp
// 测试结束时校验：
// 1. 所有订单的 filled_qty + remaining_qty == original_qty
// 2. 所有成交的 buyer/seller id 互不相同
// 3. order_id 全局唯一
```

---

## 16. 二进制协议缺少保护措施

### 发现的问题

Gateway 已在外部做协议校验，但仍有两个内部问题需要考虑：

**问题 1：无序列号 / 幂等保障——Gateway 与 NebulaX 之间的 TCP 连接如果断连重连，Gateway 重放命令，NebulaX 无法检测重复下单**

**问题 2：客户端响应丢失后无法恢复**——`order_id` 在响应中返回，如果响应丢了，客户端（Gateway）不知道新建订单的 ID，也无法取消该订单

### 改进方案

在协议头部添加客户端序列号，服务端做幂等处理：

```cpp
struct BinaryCommand {
    uint8_t  type;
    uint8_t  side;
    uint8_t  _pad[2];
    uint32_t client_seq;    // 客户端递增序列号（不同连接独立）
    uint32_t price;
    uint32_t quantity;
    uint64_t user_id;
    uint64_t order_id;
};
```

服务端维护 `per-connection last_client_seq`：

```cpp
struct ConnContext {
    // ... 现有字段 ...
    uint32_t last_client_seq = 0;  // 去重
};

// onRecv 解析时
if (cmd.client_seq <= conn->last_client_seq) {
    // 重放过期的或重复的命令，直接跳过
    // 或者返回缓存的上次响应（需要额外缓存机制）
    continue;
}
conn->last_client_seq = cmd.client_seq;
```

---

## 17. 无 DoS 防护与安全基线

### 说明

Gateway 在前端负责安全接入，因此以下场景由 Gateway 覆盖，NebulaX 不需要重复实现：

| 防护措施 | 责任方 | 备注 |
|:---------|:-------|:-----|
| TLS 信道加密 | Gateway | NebulaX 仅处理内部网络流量 |
| 客户端认证 / user_id 鉴权 | Gateway | Gateway 替换/注入 user_id |
| IP 级速率限制 | Gateway | Gateway 做 per-IP token bucket |
| 协议魔数校验 | Gateway | Gateway 做协议合法性验证 |

### NebulaX 仍需关注

| 措施 | 原因 |
|:-----|:------|
| 连接数限制 | Gateway 到 NebulaX 的连接也有限制——固定缓冲区池 64 个是硬上限 |
| 命令合法性校验 | `validateCommand` 已实现 |
| 资源使用隔离 | 各连接共享 OrderPool，一个连接的异常行为会耗尽全局资源 |

---

## 18. 构建与部署（P3）

### 发现的问题

- CMakeLists.txt 使用 `-O2`，应使用 `-O3 -march=native`
- liburing 强依赖 `/usr/local/lib/liburing.a`，不同系统路径不同
- 无 Dockerfile
- 所有硬编码值（PORT、RING_SIZE、BUF_SIZE 等）只能在编译时修改

### 改进方案

```dockerfile
# Dockerfile
FROM ubuntu:22.04 AS builder
RUN apt-get update && apt-get install -y cmake g++ liburing-dev
COPY . /app
RUN mkdir -p /app/build && cd /app/build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && make -j$(nproc)

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y liburing2
COPY --from=builder /app/build/nebulaX /usr/local/bin/
EXPOSE 2250
CMD ["nebulaX", "--io-core", "0", "--send-core", "1"]
```

CMake 构建优化：

```cmake
target_compile_options(nebulaX PRIVATE
    -O3 -march=native -mtune=native
    -Wall -Wextra -Wpedantic
    -fno-omit-frame-pointer
    -flto                # 链接时优化
)
```

---

## 19. Gateway 架构下的修正优先级

### 架构回顾

```
Client → [互联网] → Gateway → [内网] → NebulaX
                 ↑                ↑
            安全边界         信任边界
```

- Gateway 是第一个信任边界，处理 TLS、认证、IP 限速、协议校验
- NebulaX 工作在信任边界以内，不需要重复上述工作
- Gateway 和 NebulaX 之间也可能出现连接问题（内网 TCP 中断、Gateway 重启等）

### 优先级重排

| 原问题 | 优先级 | Gateway 的影响 |
|:-------|:------|:---------------|
| 优雅关闭 | P0 ← 不变 | Gateway SIGTERM NebulaX 时需 drain |
| 线程 liveness 监控 | P1 ← 不变 | 进程 hang 则 Gateway 连接全断，必须快速发现 |
| ring 背压 | P0 ← 升高 | Gateway 聚合突发 > 单个客户端，更易填满 ring |
| CQE 错误处理 | P1 ← 不变 | 内部问题，与 Gateway 无关 |
| 连接超时 + close race | P1 ← 不变 | Gateway 与 NebulaX 之间连接管理 |
| pool 耗尽降级 | P1 ← 升高 | Gateway 重试风暴加速 |
| 监控指标 | P1 ← 不变 | Gateway 看不到内部分解延迟 |
| 结构化日志 | P2 ← 不变 | 需要 trace_id 与 Gateway 关联 |
| WAL 持久化 | P2 ← 不变 | 业务需求决定 |
| 三线程分离 | P2 ← 不变 | 未来性能扩展 |
| SEND_ZC 数据丢失 | P2 ← 降低 | Gateway 重连可恢复 |
| 测试覆盖 | P2 ← 不变 | 持续投入 |
| 协议魔数/序列号 | P3 ← 降低 | Gateway 已校验 |
| DoS 防护 | P3 ← 降低 | Gateway 已防护 |
| 构建部署 | P3 ← 不变 | 运维持续改进 |

---

## 20. 总结：问题优先级总表

### P0 —— 上生产前必须解决

| # | 问题 | 预估工期 | 核心思路 |
|:-:|:-----|:--------:|:---------|
| 1 | 优雅关闭 | 1-2 天 | SIGTERM handler + drain 超时 |
| 4 | ring 背压 | 1 天 | 有界重试 + 降级 direct send + 8MB ring |

### P1 —— 2-4 周

| # | 问题 | 预估工期 | 核心思路 |
|:-:|:-----|:--------:|:---------|
| 3 | 线程 liveness 监控 | 5 天 | 心跳计数器 + watchdog + crash handler + non-blocking send |
| 5 | CQE 错误差异化 | 0.5 天 | -EAGAIN 重试，-ECONNRESET 断连，其余日志 |
| 6 | 连接空闲超时 | 0.5 天 | ConnContext last_recv_ns + 定时扫描 |
| 7 | 连接关闭竞态 | 1 天 | closing 标志 + 延迟清理 |
| 8 | pool 耗尽降级 | 3 天 | 水位监控 + NEW 限流 |
| 9 | 监控指标 | 3 天 | 无锁计数器 + 共享内存暴露 |

### P2 —— 1-2 月

| # | 问题 | 预估工期 | 核心思路 |
|:-:|:-----|:--------:|:---------|
| 10 | 结构化日志 | 2 天 | SPMC 环形 buffer + 异步文件写 |
| 11 | WAL 持久化 | 5 天 | mmap + 异步 fsync + 重启回放 |
| 12 | 三线程分离 | 2 周 | IO / Match / Send 三个独立线程 |
| 13 | SEND_ZC 重试 | 2 天 | 失败后关闭连接 + Gateway 重连 |
| 14 | 测试补充 | 持续 | 单元测试 + 确定性测试 + 一致性校验 |

### P3 —— 持续改进

| # | 问题 | 预估工期 |
|:-:|:-----|:--------:|
| 15 | TopOfBook 量聚合 | 0.5 天 |
| 16 | 协议序列号 | 2 天 |
| 17 | Dockerfile | 1 天 |
| 18 | 编译优化 -O3 -flto | 0.5 天 |

---

## 21. 多线程架构扩展审查

### 新增架构

```
Accept 线程（独立）── 负载均衡分发 fd ──→ Recv 线程池
                                              ↓
连接1 ──→ Recv 线程 1 ──→ parse ──→ cmd ──┐
连接2 ──→ Recv 线程 2 ──→ parse ──→ cmd ──┤──→ MPSC ring ──→ Match 线程 ──→ SPSC ring ──→ Send 线程
连接3 ──→ Recv 线程 3 ──→ parse ──→ cmd ──┘
```

相比当前架构（单 IO 线程），此设计带来了全新的生产级问题：

### 21.1 Cmd ring 的排队仲裁（P0）

当前 SPSCByteRing 是单生产者单消费者，**多 Recv 线程不能同时 push**。不需要重新实现 MPSC，而是在 SPSC 前面加一层排队仲裁——ticket lock：

```cpp
// 全局排队，FIFO 公平，不逐个命令抢
std::atomic<uint64_t> dispatch_ticket{0};  // 取号
std::atomic<uint64_t> now_serving{0};      // 叫号

void RecvThread::flushBatch(const BinaryCommand* batch, size_t n) {
    uint64_t my_ticket = dispatch_ticket.fetch_add(1, std::memory_order_acquire);

    // 等着叫号
    while (now_serving.load(std::memory_order_acquire) != my_ticket)
        __builtin_ia32_pause();

    // 轮到本线程写入 SPSC ring（此时是独占的，安全）
    for (size_t i = 0; i < n; ++i)
        cmd_ring_.push(&batch[i], sizeof(BinaryCommand));
    notifyMatchThread();

    // 叫下一个号
    now_serving.store(my_ticket + 1, std::memory_order_release);
}
```

**设计要点：**
- Ticket lock 确保 FIFO 公平，不会出现某个 Recv 线程饿死
- 临界区极短（批量 memcpy 几十到几百字节），排队等待时间约几百纳秒
- 不扩展 SPSC 代码，不做无锁 MPSC（避免 ABA + 内存序调试地狱）
- Recv 线程**攒足 batch 再 flush**（不是逐条命令抢），抢排队频率可控

**生产风险：**
- 攒 batch 引入了延迟（等 N 条才 flush），需要平衡 batch 大小和延迟要求
- ticket lock 在高竞争下仍有 cache bouncing，但比 CAS 自旋好（因为排队公平，不会有多余的 exchange 冲突）
- 需要在每个 Recv 线程的批量大小、flush 频率和 cmd ring 容量之间做调优

### 21.2 Accept → Recv 线程连接分发（P1）

Accept 线程拿到新 fd 后，要交给一个 Recv 线程接管。

```cpp
void onAccept(int client_fd) {
    int idx = next_recv_thread_.fetch_add(1) % thread_count;
    // 怎么把 fd 交给线程 idx？
    // 方案 A：eventfd + 共享队列（Recv 线程从队列取 fd）
    // 方案 B：直接由 Accept 线程提交 recv（但 Accept 线程没有 io_uring 实例）
}
```

**生产风险：**
- 方案 A 需要在 Rev 线程和 Accept 线程之间加一个连接，引入了新的跨线程通信
- 负载均衡策略（轮询/最小连接/最小负载）影响吞吐
- Accepted 的 fd 需要在 Recv 线程被创建或销毁时重新分配

### 21.3 Recv 线程池生命周期管理（P1）

```cpp
// Recv 线程需要支持：
// 1. 启动时创建固定数量线程，每个绑定自己的 io_uring 实例
// 2. 运行时停止某个线程（连接重新分配）
// 3. 线程 crash 后的检测与恢复
// 4. 优雅关闭时所有线程安全退出

class RecvThreadPool {
    std::vector<std::thread> threads_;
    std::vector<IoUringPoller> pollers_;  // 每个线程有自己的 io_uring
    std::vector<ConnPinner> conns_;       // fd → thread 映射
public:
    // 停止线程 i：先将它的连接迁移到其他线程
    void stopWorker(int i);
    // 检查所有线程 liveness
    void checkHealth();
};
```

**生产风险：**
- 一个 Recv 线程 crash，它上面的所有连接无人处理
- 需要连接迁移机制（将 fd 重新分配给其他 Recv 线程）
- 迁移过程中可能丢数据（已 recv 但未解析的 TCP 数据）

### 21.4 两级背压（P1）

新架构中有两个队列，任意一个满了都会导致连锁反应：

```
Recv 线程 → MPSC cmd ring → Match 线程 → SPSC resp ring → Send 线程
                                  ↑ 满                 ↑ 满
Match 线程阻塞住                Recv 线程卡住          IO 线程卡住（当前已有方案）
```

**生产风险：**
- **cmd ring 满时**：Recv 线程无法 flush batch，batch 在本地积压。如果连续几次 flush 失败，需停止该连接的 recv，让 TCP 窗口关闭，自然回压到 Gateway
- **resp ring 满时**：Match 线程无法推送响应，cmd ring 继而也满，形成两级阻塞链条

**改进方案：**

```cpp
// 两级背压的不同处理策略：

// cmd ring 满 → Recv 线程 batch 积压，超过阈值后停止该连接 recv
void RecvThread::onCmdRingFull(int fd) {
    if (local_batch_count > MAX_PENDING_BATCH) {
        stopRecv(fd);  // 暂停 recv，TCP 窗口关闭
                       // 客户端 send 被背压
    }
}

// resp ring 满 → Match 线程降级到 direct send
void MatchThread::onRespRingFull(int fd, const std::vector<Response>& buf) {
    // 回退到 plain send（当前已有方案）
}

// ticket lock 模式下 cmd ring 满的概率更低：
// Recv 线程攒 batch 再 flush，如果 single SPSC ring 容量够，
// 排队写入 + 批量消费的节奏自然平滑
```

### 21.5 连接与线程的调度耦合（P2）

```cpp
// 核心设计问题：一个连接在整个生命周期中必须绑定到同一个 Recv 线程
// 否则该连接的 TCP 数据会被多个线程读取，顺序不可保证

// Recv 线程销毁前需要迁移连接的步骤：
// 1. 停止该线程的 recv
// 2. 关闭原始 io_uring CQE（防止旧线程再读到新数据）
// 3. 从该线程的 conns_ 中读出所有连接
// 4. 重新分配给其他 Recv 线程
// 5. 安全销毁原线程
```

**生产风险：**
- 连接和线程的绑定关系需要全局映射（`fd → thread_id`），否则 Send 线程发 `RSP_CLOSE` 时不知道通知谁
- 动态扩缩容时连接迁移成本高，涉及 TCP 数据完整性

### 21.6 新架构下的风险与当前审查的对比

| 风险 | 当前架构 | 新架构 | 当前审查是否覆盖 |
|:-----|:---------|:-------|:----------------|
| 线程 crash | 2 个线程 | N+2 个线程 | ❌ 只覆盖了双线程场景 |
| 队列背压 | 1 级（resp ring）| 2 级（cmd + resp）| ❌ 缺少 cmd ring |
| 跨线程通信 | SPSC（简单）| MPSC（复杂 10 倍）| ❌ 未涉及 |
| 连接分配 | 单线程自然接管 | 需要负载均衡 | ❌ 未涉及 |
| 线程健康检查 | 2 个心跳 | N 个 Recv 线程 | ⚠️ 方案通用但复杂度增加 |
| 优雅关闭 | join 两个线程 | join N+2 个线程 + 连接迁移 | ❌ 需要修改 |

---

**结语：** NebulaX 的性能指标（13.9M QPS / 5µs P50 延迟）在原型阶段已非常出色。从原型到生产就绪，需要补的不是性能优化，而是可靠性和可运维性的基础设施——优雅关闭、背压保护、监控告警、数据持久化。这部分工程工作量和性能优化的工作量大致相当，但对于真实交易系统来说不可或缺。
