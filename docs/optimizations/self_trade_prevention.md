# 自成交防护（Self-Trade Prevention）

**发现：** 2026-05-21 | **修复：** Phase 4 epoll reactor 同期

## 问题

`matchBuyOrder` 和 `matchSellOrder` 撮合时只检查价格和时间优先级，不检查 `user_id`。如果同一个用户同时在买卖两侧有盘口单，引擎会自成交——`buyer_id == seller_id`，这在现实交易系统中通常是被禁止的行为。

## 修复

在 `OrderBook::getBestBid()` / `getBestAsk()` 加可选的 `exclude_user_id` 参数：

```
Order* getBestBid(uint64_t exclude_user_id = 0);
Order* getBestAsk(uint64_t exclude_user_id = 0);
```

匹配逻辑改为：

- `matchBuyOrder` 调用 `getBestAsk(order.user_id)` — 跳过自己的卖单
- `matchSellOrder` 调用 `getBestBid(order.user_id)` — 跳过自己的买单

`exclude_user_id = 0`（默认值）走快速路径，与修改前一致，零开销。

## 涉及文件

| 文件 | 改动 |
|------|------|
| `include/order_book.h` | 函数签名加参数 |
| `src/order_book.cpp` | 添加 `exclude_user_id` 过滤逻辑 |
| `src/matching_engine.cpp` | 传递 `order.user_id` 给 getBestBid/getBestAsk |

## 效果

相同 user_id 的买卖单在盘口共存但不成交。其他用户可以与该用户的盘口单正常成交。

## 验证

基准测试（不同 UID）：12.1M QPS，无退化。
正确性测试：132/132 PASS。
