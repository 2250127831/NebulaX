# NebulaX 性能监测报告 — Phase 6 内存池

**日期：** 2026-05-23 | **系统：** Ubuntu 22.04 | **网络：** 127.0.0.1 loopback TCP

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

与前序保持一致：同一命令序列（50% NEW + 25% CANCEL + 25% BOOK，价格区间 10000~14999 重叠），Pipeline 模式每轮 50M 条（4 连接平分），重复 3 轮。10K 条预热消除冷 cache 影响。

**QPS 计算说明：** 采用 `total_cmds / max_wall_us`（总命令数 / 最慢 worker 耗时）。

---

## 架构变更

Phase 5 rev2 使用 `std::list<Order>` + `std::unordered_map<uint64_t, Order*>`，每条 Order 独立 heap 分配。Phase 6 替换为预分配内存池 + 自定义哈希表 + 侵入式链表。

```
数据结构对比

Phase 5 rev2                    Phase 6
─────────────                   ─────────────
std::list<Order>                侵入式链表 prev_idx/next_idx
  → 每条 Order 独立 new/delete    → OrderPool 预分配，空闲链表取

std::unordered_map               OrderMap 自定义哈希表
  → heap 分配 node（含 rehash）    → 平面 node pool + bucket 数组预分配
  → reserve() 只预分配 bucket      → 免 heap，免 rehash
  → 必须传 key 查询               → removeOrder(Order*) 跳过查询

std::map<uint32_t, PriceLevel>   不变（rb_tree 开销 ~0.5%）
```

### OrderPool

`OrderPool` 是定长数组 + 空闲链表。构造时分配连续内存（默认 4M 条）。`alloc()` 从空闲链表取头节点，`free(Order*)` 归还。`at(idx)` 返回 `&storage_[idx]`，零开销。

```cpp
template <uint32_t Capacity = 4'000'000>
class OrderPool {
    alignas(64) Order storage_[Capacity];
    uint32_t free_head_;  // 空闲链表头
};
```

### OrderMap

`OrderMap` 是自定义哈希表，分离链接法 + 平面 node pool（预分配 4M node，无 per-entry heap 分配）。bucket 数组大小取质数（默认 4,194,317 = 4M × 1.05），乘法哈希。

```cpp
struct OrderMapNode {
    uint64_t key;
    uint32_t value;   // OrderPool index
    uint32_t next;    // 链式冲突解决
};

class OrderMap {
    std::vector<uint32_t> buckets_;
    OrderMapNode* nodes_;        // 预分配 node pool
    uint32_t node_count_;
    uint32_t free_node_;         // 空闲链表
};
```

`value` 存的是 OrderPool index（uint32_t），不是指针。Order 被 pool 管理，地址会变——index 是稳定标识。

### 侵入式链表

`Order` 结构体新增 `prev_idx / next_idx`（uint32_t），`PriceLevel` 存储头尾 index 而非指针。Order 在 PriceLevel 间的增删改为 index 操作。

Order 保持在 64 字节（一条 cache line）：

```
offset  size  field
     0     8  client_fd
     8     8  order_id
    16     8  price
    24     8  quantity
    32     4  remaining
    36     4  type/flags（enum 压缩）
    40     4  prev_idx     ← 新增
    44     4  next_idx     ← 新增
    48     4  pool_next_free ← 新增
    52    12  padding
    64
```

### 文件变更

| 文件 | 变更 |
|:----|------|
| include/order.h | 新增 prev_idx/next_idx/pool_next_free，static_assert 64 字节 |
| include/order_pool.h | 新建，OrderPool 模板类 |
| include/order_map.h | 新建，OrderMap 类 |
| include/order_book.h | 替换类型，新增 pool + map 成员 |
| src/order_book.cpp | 全量重写 |
| src/matching_engine.cpp | 适配 addOrder 返回值，使用 removeOrder(Order*) |
| include/protocol.h | 新增 POOL_FULL 错误码 |
| src/tcp_server.cpp | closeConnection 重排序 |

---

## 对比数据

### 关键指标一览

| 指标 | Phase 5 rev2 | Phase 6 |
|:----|:------------:|:-------:|
| Pipeline sustained (50M) | 7.2M | **12.2M** |
| Ping-pong avg | 8µs | 8µs |
| IPC | 0.99 | **1.11** |
| cache-misses | 953M | **713M** |
| ctx/s | 1227 | 2468 |

Pipeline 持续负载 QPS 提升 **+69%**。cache-misses 下降 **25%**，IPC 从 0.99 升至 1.11，表明 memory latency 瓶颈显著缓解。ctx/s 上升是因发送线程在更高吞吐下更活跃。

### Phase 6 逐轮明细

#### Pipeline（50M sustained）

| 轮次 | QPS |
|:----|:---:|
| 1 | 11,329,057 |
| 2 | 13,246,721 |
| 3 | 12,022,682 |
| **均** | **12,199,486** |

三轮波动较大（11.3M~13.2M），可能来自系统频率调整、Turbo Boost 状态差异或 NUMA 影响。平均值 12.2M 代表稳态性能。

#### Ping-pong

| 轮次 | avg | P50 | P99 | P999 | QPS |
|:----|----:|----:|----:|----:|----:|
| Phase 5 rev2（参照）| 8µs | 8µs | 12µs | 26µs | 119,060 |
| Phase 6 | 8µs | 8µs | 12µs | 26µs | ~120K |

Ping-pong 无变化——数据结构替换不影响单命令延迟，瓶颈在 TCP 往返。

---

## 火焰图（Sustained 50M）

Pipeline 负载下 CPU 分布对比：

| 分类 | Phase 5 rev2 | Phase 6 | 说明 |
|:----|:------------:|:-------:|------|
| processNewOrder | 48.65% | **37.66%** | -11pp，pool 消除 heap |
| addOrder | 33.27% | **18.25%** | -15pp，OrderMap+pool |
| matchBuyOrder | — | **6.81%** | 拆为独立函数 |
| removeOrder(Order*) | — | **4.08%** | 跳过 hash 查询 |
| operator new | 2.79% | **0.56%** | heap 分配几乎消除 |
| unordered_map 操作 | ~5% | **0%** | 已替换 |
| send 路径 | ~6% | ~9.68% | 吞吐提升后主动增加 |

operator new 从 2.79% 降至 0.56%——残存分配来自 std::map（PriceLevel 增删）。unordered_map rehash 开销彻底消除。

---

## 硬件事件

### Pipeline（burst 500K）

| 事件 | Phase 5 rev2 | Phase 6 |
|:----|:------------:|:-------:|
| IPC | 0.99 | **1.11** |
| cache-misses | 953,000,000 | **713,000,000** |
| L1-dcache-load-misses | ~18.4M | ~17.5M |
| LL-load-misses | — | **-25%** |

### Sustained 50M

| 事件 | Phase 5 rev2 | Phase 6 |
|:----|:------------:|:-------:|
| ctx/s | 1227 | **2468** |
| send() CPU | 6.92% | **9.68%** |

send 路径 CPU 占比上升是因为吞吐提升后发送量更大，不是退化。

---

## 后续方向

| 阶段 | 内容 | 目的 |
|:----|------|------|
| Phase 7 | io_uring | send/recv syscall 批量化，进一步降低 syscall 开销 |
