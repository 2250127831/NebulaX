#!/usr/bin/env bash
# 测试停机快照：下单 → SIGTERM → 重启 → 检查盘口恢复
set -e

cd ~/NebulaX/build

rm -f /tmp/nebulaX_snapshot.dat

# 启动服务端
taskset -c 0-1 ./nebulaX 2250 --io-core 0 --send-core 1 &
SERVER_PID=$!
sleep 2

# 发少量订单
echo "Sending orders..."
taskset -c 2 ~/NebulaX/benchmark/benchmark_client 127.0.0.1 2250 -p 2>/dev/null | tail -1
echo "Orders sent."

# 发 SIGTERM 优雅关闭
kill -TERM $SERVER_PID
wait $SERVER_PID 2>/dev/null
echo "Server stopped."

# 检查快照
if [ -f /tmp/nebulaX_snapshot.dat ]; then
    SNAP_SIZE=$(stat -c%s /tmp/nebulaX_snapshot.dat)
    echo "Snapshot saved: $SNAP_SIZE bytes"
else
    echo "ERROR: snapshot not found!"
    exit 1
fi

# 重启并验证
echo "Restarting server..."
taskset -c 0-1 ./nebulaX 2250 --io-core 0 --send-core 1 &
SERVER_PID=$!
sleep 2

# 通过 BOOK 命令确认盘口不为空
echo "Verifying order book..."
HEX=$(echo -n -e '\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' | nc -w1 127.0.0.1 2250 | xxd | head -3)
echo "BOOK response: $HEX"

kill -TERM $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
echo "Done."
