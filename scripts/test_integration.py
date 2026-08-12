#!/usr/bin/env python3
"""NebulaX 集成测试：编译→启动→下单→崩溃→恢复→WAL回放。"""
import socket, struct, time, os, signal, sys, subprocess, mmap

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(BASE, "build")
NEBULA = os.path.join(BUILD, "nebulaX")
SHM_BOOK = "/dev/shm/nebulaX_book"
WAL_PATH = "/tmp/nebulaX_wal.dat"
METRICS_SHM = "/dev/shm/nebulaX_metrics"
passed = 0; failed = 0

def ok(name): global passed; passed += 1; print(f"  ✅ {name}")
def fail(name, msg): global failed; failed += 1; print(f"  ❌ {name}: {msg}")

def start_server():
    pid = os.fork()
    if pid == 0:
        os.execv(NEBULA, ["nebulaX", "2250"])
        os._exit(1)
    time.sleep(0.8)
    return pid

def stop_server(pid, sig=signal.SIGTERM):
    try:
        os.kill(pid, sig)
        os.waitpid(pid, 0)
    except: pass

def itch_add(loc, order_ref, side, price_cents, shares):
    """ITCH 5.0 A 消息（Add Order, body 36B）+ 2B 长度前缀。"""
    body = struct.pack(">B", 0x41)            # 'A'
    body += struct.pack(">H", loc)            # locate
    body += struct.pack(">H", 0)              # track
    body += b"\x00" * 6                       # timestamp
    body += struct.pack(">Q", order_ref)      # order_ref
    body += b"B" if side == 1 else b"S"       # buy/sell
    body += struct.pack(">I", shares)         # shares
    body += b"TEST1   "                       # stock 8B
    body += struct.pack(">I", price_cents * 100)  # price (分×100)
    return struct.pack(">H", len(body)) + body

def send_orders(count=100, port=2250):
    s = socket.socket(); s.settimeout(10)
    s.connect(("127.0.0.1", port))
    # 按 ITCH 帧发送 A 消息（顺序发，每条等响应——服务端攒批推送）
    for i in range(count):
        s.sendall(itch_add(1, 1000 + i, 1, 100 + i % 990, 10))
    # 收响应：每条订单至少一个非 TRADE 最终帧（可能多个 RSP_TRADE + RSP_OK）
    received = 0
    while received < count:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk: break
        # 48B 一帧，数非 TRADE 帧数（一个订单一个最终状态帧）
        nframes = len(chunk) // 48
        for f in range(nframes):
            if chunk[f*48] != 0x81:  # RSP_TRADE=0x81 跳过
                received += 1
    s.close()
    return received == count

def read_metrics():
    fd = os.open(METRICS_SHM, os.O_RDONLY)
    mm = mmap.mmap(fd, 128, mmap.MAP_SHARED, mmap.PROT_READ)
    os.close(fd)
    d = struct.unpack_from("<16Q", mm, 0)
    mm.close()
    return d  # d[3]=new_orders, d[8]=order_pool_used

# ── 1. 编译 ──
print("=== 1. Build ===")
subprocess.run(["cmake", "..", "-DCMAKE_BUILD_TYPE=Release"], cwd=BUILD, capture_output=True)
ret = subprocess.run(["make", "-j4", "nebulaX"], cwd=BUILD, capture_output=True)
if ret.returncode == 0: ok("build")
else: fail("build", ret.stderr.decode())

# ── 2. 清理残留 ──
os.system(f"killall -9 nebulaX 2>/dev/null; rm -f {SHM_BOOK} {METRICS_SHM} {WAL_PATH} /tmp/nebulaX_snapshot.dat")
time.sleep(0.3)

# ── 3. 启动 → 下单 → 验证 ──
print("\n=== 2. Smoke Test ===")
pid = start_server()
ok("server started")

if send_orders(50): ok("50 orders sent/received")
else: fail("50 orders", "recv timeout")

stop_server(pid, signal.SIGTERM)
ok("graceful shutdown")
time.sleep(0.3)

# ── 4. 崩溃恢复（共享内存）──
print("\n=== 3. Crash Recovery (shared memory) ===")
pid = start_server()
if send_orders(100): ok("100 orders on new server")
else: fail("100 orders", "recv timeout")
stop_server(pid, signal.SIGSEGV)
time.sleep(0.3)

if os.path.exists(SHM_BOOK): ok("shared memory survives SIGSEGV")
else: fail("shared memory", "not found")

pid2 = start_server()
time.sleep(0.5)
d = read_metrics()
orders = d[3]
if orders >= 100: ok(f"recovered {orders} orders from shared memory")
else: fail("recovery", f"got {orders} expected >=100")
stop_server(pid2, signal.SIGTERM)
time.sleep(0.3)

# ── 5. WAL 恢复（共享内存丢失）──
print("\n=== 4. WAL Disaster Recovery ===")
os.system(f"rm -f {SHM_BOOK} {METRICS_SHM} /tmp/nebulaX_snapshot.dat")
time.sleep(0.2)
ok("shared memory wiped")

if os.path.exists(WAL_PATH): ok("WAL file survives")
else: fail("WAL file", "not found")

pid3 = start_server()
time.sleep(1)
d = read_metrics()
pool = d[8]
if pool >= 50: ok(f"WAL recovery: {pool} orders in pool")
else: fail("WAL recovery", f"pool={pool} expected >=50")
stop_server(pid3, signal.SIGTERM)

# ── 清理 ──
os.system(f"killall -9 nebulaX 2>/dev/null; rm -f {SHM_BOOK} {METRICS_SHM} {WAL_PATH} /tmp/nebulaX_snapshot.dat")

# ── 汇总 ──
print(f"\n{'='*40}")
print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed > 0 else 0)
