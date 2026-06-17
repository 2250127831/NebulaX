#!/usr/bin/env python3
"""
L2 真实行情回放

数据来源: akshare stock_zh_a_tick_tx_js
日期: 2026-06-17（最近交易日）
标的: 平安银行、万科A、格力电器、贵州茅台、招商银行等 20 只沪深股票
原始交易: 69633 条逐笔成交
生成订单: ~80000 笔（含 ~20% 撤单）

用法:
  python3 scripts/l2_replay.py [port]         # 批量回放（全速）
  python3 scripts/l2_replay.py [port] --csv   # 仅生成CSV
"""
import socket, struct, time, sys, os, csv, random

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 2250
CSV_PATH = "/tmp/l2_replay.csv"
BATCH = 4000

def fetch():
    import akshare as ak
    symbols = ["sz000001","sz000002","sz000651","sh600519","sh600036",
               "sz000858","sz002415","sh601318","sz300750","sh600887",
               "sz002594","sh600900","sz000333","sh601166","sz002714",
               "sh600030","sh600585","sz002304","sh601398","sz000568"]
    rows = []
    for sym in symbols:
        try:
            df = ak.stock_zh_a_tick_tx_js(sym)
            for _, r in df.iterrows():
                t = r["成交时间"]
                h,m,s = t.split(":")
                rows.append((int(h)*3600+int(m)*60+int(s), r["成交价格"], int(r["成交量"])))
        except: pass
    rows.sort()
    return rows

def gen(ticks):
    random.seed(42)
    uid = 0; active = []; orders = []
    for ms, price, vol in ticks:
        uid += 1
        p = int(float(price) * 100)
        orders.append((ms, "NEW", 1, p, vol, uid))
        active.append(uid)
        if len(active) > 20 and random.random() < 0.2:
            idx = random.randint(0, len(active)-1)
            orders.append((ms, "CANCEL", 0, 0, 0, active.pop(idx)))
    return orders

def run():
    if "--csv" in sys.argv:
        t0 = time.time()
        ticks = fetch()
        orders = gen(ticks)
        with open(CSV_PATH, "w") as f:
            w = csv.writer(f)
            w.writerow(["t","type","side","price","qty","uid"])
            for o in orders: w.writerow(o)
        print(f"CSV: {len(orders)} 条  ({time.time()-t0:.1f}s)")
        return

    if not os.path.exists(CSV_PATH):
        print("生成 CSV...")
        t0 = time.time()
        ticks = fetch()
        orders = gen(ticks)
        with open(CSV_PATH, "w") as f:
            w = csv.writer(f); w.writerow(["t","type","side","price","qty","uid"])
            for o in orders: w.writerow(o)
        print(f"  完成 ({time.time()-t0:.1f}s)")

    # 批量发送所有订单
    with open(CSV_PATH) as f:
        lines = [l for l in f if not l.startswith("ms")][:100000]

    buf = bytearray()
    for line in lines:
        p = line.strip().split(",")
        if p[1] == "NEW":
            buf.extend(struct.pack("<BBxxII4xQQ", 1,
                1 if p[2]=="1" else 2, int(p[3]), int(p[4]), int(p[5]), 0))
        else:
            buf.extend(struct.pack("<BBxxII4xQQ", 2, 0, 0, 0, int(p[5]), 0))

    total_orders = len(buf) // 32
    print(f"回放: {total_orders} 笔 (分批{BATCH})")

    s = socket.socket(); s.settimeout(120)
    s.connect(("127.0.0.1", PORT))
    t0 = time.time()
    off = 0; ok = 0
    while off < len(buf):
        chunk = buf[off:off + BATCH * 32]
        try:
            s.sendall(bytes(chunk))
            need = len(chunk) // 32 * 48
            while need > 0:
                c = s.recv(min(need, 65536))
                if not c: break
                need -= len(c)
            ok += len(chunk) // 32
        except: break
        off += len(chunk)
    t = time.time() - t0
    s.close()
    print(f"  成功: {ok}/{total_orders}  {t:.1f}s  {ok/t:.0f} QPS")
    return ok, t

if __name__ == "__main__":
    run()
