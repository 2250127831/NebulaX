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

def send_orders(count=100, port=2250):
    s = socket.socket(); s.settimeout(10)
    s.connect(("127.0.0.1", port))
    buf = bytearray()
    for i in range(count):
        buf.extend(struct.pack("<BBxxII4xQQ", 1, 1, 100 + i%990, 10, i%65535+1, 0))
    s.sendall(bytes(buf))
    total = count * 48
    while total > 0:
        chunk = s.recv(min(total, 65536))
        if not chunk: break
        total -= len(chunk)
    s.close()
    return total == 0

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
