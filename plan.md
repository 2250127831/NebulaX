# NebulaX 工程化推进计划

## 已完成

- [x] 优雅关闭（SIGTERM handler + drain + io_shutdown_done 标志）
- [x] Ring 背压（空间不足时自动降级 plain send）
- [x] 停机快照（优雅关闭时保存订单簿，重启后恢复）
- [x] CQE 错误差异化（EAGAIN 重试、ECONNRESET/EPIPE 正常断连、其他日志）
- [x] 连接管理（TCP keepalive 死连检测 + ack 关闭竞态修复）
- [x] 监控指标（Per-thread 计数器 + shm_open 共享内存 + read_metrics.py 采集脚本）
- [x] 结构化日志（MPSC 环形缓冲区 + 分级时间戳日志 + 消费者线程）
- [x] **WAL + 崩溃恢复 + 线程存活检测**
   - 共享内存 OrderPool / TradePool / 心跳计数器
   - WAL 512MB 单文件环形 mmap，满时 fork checkpoint
   - 三层恢复：共享内存 / checkpoint+WAL / WAL 幂等回放
   - SIGSEGV handler: fdatasync(WAL) + _exit（不 shm_unlink）
   - IO/Send 线程心跳检测
- [x] **单元测试框架**
   - test_unit: MPSCRing / OrderPool / WalEntry 测试

## 说明

工程化计划已全部完成。测试覆盖可随开发持续补充。
