#!/usr/bin/env python3
import socket, struct, time, sys, os, signal

port = int(sys.argv[1]) if len(sys.argv) > 1 else 2254

# spawn nebulaX
pid = os.fork()
if pid == 0:
    os.execv("/home/qiwang/NebulaX/build/nebulaX", ["nebulaX", str(port)])
    os._exit(1)

time.sleep(0.5)

ok = True
s = socket.socket()
s.settimeout(3)
try:
    s.connect(("127.0.0.1", port))

    def cmd_new(side, price, qty, uid):
        return struct.pack("<BBxxII4xQQ", 1, 0x01 if side == "B" else 0x02, price, qty, uid, 0)

    def cmd_cancel(oid, uid):
        return struct.pack("<BBxxII4xQQ", 2, 0, 0, 0, uid, oid)

    # buy 100@10 uid=1
    s.send(cmd_new("B", 100, 10, 1))
    r = s.recv(48)
    assert r[0] == 0x82, f"expected RSP_OK(0x82) got {hex(r[0])}"
    print("NEW_B: OK")

    # sell 100@5 uid=2 → matches 5
    s.send(cmd_new("S", 100, 5, 2))
    r = s.recv(48)
    assert r[0] == 0x81, f"expected RSP_TRADE(0x81) got {hex(r[0])}"
    print("TRADE: OK")
    r = s.recv(48)
    assert r[0] in (0x82, 0x83), f"expected RSP_OK(0x82) or RSP_FILLED(0x83) got {hex(r[0])}"
    print("SELL_ACK:", hex(r[0]))

    # buy 99@5 uid=3 → no match, rests
    s.send(cmd_new("B", 99, 5, 3))
    r = s.recv(48)
    assert r[0] == 0x82, f"expected RSP_OK(0x82) got {hex(r[0])}"
    print("NEW_B2: OK")

    # cancel order_id=1 uid=1
    s.send(cmd_cancel(1, 1))
    r = s.recv(48)
    assert r[0] == 0x84, f"expected RSP_CANCELLED(0x84) got {hex(r[0])}"
    print("CANCEL: OK")

    print("PASS")
except Exception as e:
    print(f"FAIL: {e}")
    ok = False
finally:
    s.close()
    os.kill(pid, signal.SIGTERM)
    os.waitpid(pid, 0)

sys.exit(0 if ok else 1)
