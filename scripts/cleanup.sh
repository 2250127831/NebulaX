#!/bin/bash
# NebulaX 一键清理：杀进程 + 删 WAL / SHM / 快照 / 日志
set -e

echo "=== NebulaX Cleanup ==="

# 杀进程
if pgrep -x nebulaX > /dev/null 2>&1; then
    echo "[1/5] killing nebulaX..."
    pkill -9 -x nebulaX
else
    echo "[1/5] nebulaX not running"
fi

# 删 WAL
echo "[2/5] removing WAL..."
rm -f /tmp/nebulaX_wal.dat

# 删快照
echo "[3/5] removing snapshot..."
rm -f /tmp/nebulaX_snapshot.dat

# 删共享内存
echo "[4/5] removing shared memory..."
rm -f /dev/shm/nebulaX_book /dev/shm/nebulaX_metrics

# 删日志
echo "[5/5] removing logs..."
rm -f /home/qiwang/NebulaX/logs/nebulaX.log

echo "=== Done ==="
