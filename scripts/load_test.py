#!/usr/bin/env python3
"""NebulaX 负载测试 —— 发送 21 万笔订单使 pool 使用率达到 ~5%"""
import socket, struct, signal, os, time, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 2255

pid = os.fork()
if pid == 0:
    os.execv("/home/qiwang/NebulaX/build/nebulaX", ["nebulaX", str(port)])
    os._exit(1)

time.sleep(0.5)

s = socket.socket()
s.settimeout(60)
try:
    s.connect(("127.0.0.1", port))
    t0 = time.time()

    for batch in range(21):
        buf = bytearray()
        for i in range(10000):
            uid = (batch * 10000 + i) % 65535 + 1
            price = 100 + uid % 990
            buf.extend(struct.pack("<BBxxII4xQQ", 1, 1, price, 10, uid, 0))

        s.sendall(bytes(buf))
        need = 10000 * 48
        while need > 0:
            chunk = s.recv(need)
            if not chunk: break
            need -= len(chunk)

        t = time.time() - t0
        print(f"batch {batch+1}/21  {batch * 10000}/210k  {t:.1f}s")

    elapsed = time.time() - t0
    print(f"\nall done in {elapsed:.1f}s ({210000/elapsed:.0f} orders/s)")
except Exception as e:
    print(f"FAIL: {e}")
finally:
    s.close()
    os.kill(pid, signal.SIGTERM)
    os.waitpid(pid, 0)
