# NebulaX 工程化推进计划（由易到难）

## 已完成

- [x] 优雅关闭（SIGTERM handler + drain + io_shutdown_done 标志）
- [x] Ring 背压（空间不足时自动降级 plain send）
- [x] 停机快照（优雅退出时保存订单簿，重启后恢复）
- [x] CQE 错误差异化（EAGAIN 重试、ECONNRESET/EPIPE 正常断连、其他日志）
- [x] 连接管理（TCP keepalive 死连检测 + ack 关闭竞态修复）
- [x] 监控指标（Per-thread 计数器 + shm_open 共享内存 + read_metrics.py 采集脚本）

### 3. 结构化日志（~2天）

- 独立 SPSC 环形缓冲区 + 日志线程消费
- 日志级别（FATAL/ERROR/WARN/INFO/DEBUG）
- 热路径不执行 IO 或格式化

### 4. 线程存活检测（~2天）

- 线程心跳计数器（per-thread relax store）
- Watchdog（主线程轮询，超时 3s 触发紧急 dump + SIGTERM）
- Send 线程无响应由进程内部重启
- IO 线程无响应则转储退出，由外部 supervisor 拉起

### 5. WAL 最小可用版（~3天）

IO 线程 crash 时优雅关闭的快照跑不了，需要 WAL 兜底。
- mmap 日志文件，每条操作 append（NEW/CANCEL）
- 文件满时全量 checkpoint
- 启动时回放重建订单簿

### 6. 测试补充（持续）

- 单元测试（组件边界条件）
- 状态一致性校验（quantity 守恒）
- 压测程序模拟异常路径（ring 降级验证）
