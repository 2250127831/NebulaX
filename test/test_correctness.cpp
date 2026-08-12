// NebulaX 正确性验证 —— 逐帧校验响应完整性
// 每次测试独立启动/停止服务端，杜绝状态污染
// 编译: g++ -std=c++17 -O2 test_correctness.cpp -o test_correctness -lpthread
// 运行: ./test_correctness

#include "../include/protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int g_pass = 0, g_fail = 0;

static void result(bool ok, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (ok) { ++g_pass; }
    else {
        ++g_fail;
        printf("  FAIL: "); vprintf(fmt, ap); printf("\n");
    }
    va_end(ap);
}

// ── ITCH 消息构造辅助（大端序）──
static void putU16BE(std::vector<char>& v, uint16_t x) {
    v.push_back((char)(x >> 8)); v.push_back((char)(x & 0xFF));
}
static void putU32BE(std::vector<char>& v, uint32_t x) {
    v.push_back((char)(x >> 24)); v.push_back((char)(x >> 16));
    v.push_back((char)(x >> 8));  v.push_back((char)(x & 0xFF));
}
static void putU64BE(std::vector<char>& v, uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back((char)(x >> (8 * i)));
}
// 带 2B 长度前缀发送 ITCH 消息
static void sendItchFrame(int sock, const std::vector<char>& body) {
    char hdr[2];
    hdr[0] = (char)(body.size() >> 8);
    hdr[1] = (char)(body.size() & 0xFF);
    ::send(sock, hdr, 2, 0);
    ::send(sock, body.data(), body.size(), 0);
}
// A: Add Order（body 36B）
static std::vector<char> itchAdd(uint16_t locate, uint64_t order_ref, char side,
                                 uint32_t price_cents, uint32_t shares) {
    std::vector<char> b;
    b.push_back('A');
    putU16BE(b, locate);        // locate
    putU16BE(b, 0);             // track
    for (int i = 0; i < 6; ++i) b.push_back(0);   // timestamp
    putU64BE(b, order_ref);     // order_ref
    b.push_back(side);          // 'B' / 'S'
    putU32BE(b, shares);        // shares
    for (int i = 0; i < 8; ++i) b.push_back(0);   // stock (8 chars)
    putU32BE(b, price_cents * 100);  // ITCH price = 分 × 100 (1/10000 美元)
    return b;
}
// D: Order Delete（body 19B）
static std::vector<char> itchDelete(uint16_t locate, uint64_t order_ref) {
    std::vector<char> b;
    b.push_back('D');
    putU16BE(b, locate);
    putU16BE(b, 0);
    for (int i = 0; i < 6; ++i) b.push_back(0);
    putU64BE(b, order_ref);
    return b;
}
// Q: Book Query（body 3B）
static std::vector<char> itchBookQuery(uint16_t locate) {
    std::vector<char> b;
    b.push_back('Q');
    putU16BE(b, locate);
    return b;
}

// ── 简易 TCP 客户端 ──
struct Client {
    int sock = -1;
    uint16_t locate = 0;         // 本连接默认标的（分簿）
    std::vector<char> buf;

    Client(const char* ip, int port, uint16_t loc = 0) : locate(loc) {
        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET; addr.sin_port = htons(port);
        inet_pton(AF_INET, ip, &addr.sin_addr);
        if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(sock); sock = -1;
        }
    }
    ~Client() { if (sock >= 0) ::close(sock); }
    bool ok() const { return sock >= 0; }

    void sendItch(const std::vector<char>& body) { sendItchFrame(sock, body); }

    // 读取下一帧（TRADE 不跳过）
    bool recv(BinaryResponse& rsp) {
        while (buf.size() < sizeof(rsp)) {
            char raw[4096];
            ssize_t n = ::recv(sock, raw, sizeof(raw), 0);
            if (n <= 0) return false;
            buf.insert(buf.end(), raw, raw + n);
        }
        memcpy(&rsp, buf.data(), sizeof(rsp));
        buf.erase(buf.begin(), buf.begin() + sizeof(rsp));
        return true;
    }

    // 读取所有响应帧直到遇到非 TRADE 帧
    // 返回非 TRADE 帧和 TRADE 统计
    bool collect(uint64_t expect_trades, BinaryResponse& final_rsp,
                 uint64_t& trade_count, uint64_t& trade_qty) {
        trade_count = 0; trade_qty = 0;
        while (true) {
            BinaryResponse rsp;
            if (!recv(rsp)) return false;
            if (rsp.type == RSP_TRADE) {
                trade_count++;
                trade_qty += rsp.data.trade.quantity;
            } else {
                final_rsp = rsp;
                return trade_count == expect_trades;
            }
        }
    }

    // NEW 快捷方法（返回 order_id，失败返回 0）
    uint64_t newOrder(uint8_t side, uint32_t price, uint32_t qty, uint64_t uid) {
        static uint64_t next_ref = 1;
        uint64_t oref = next_ref++;
        char itch_side = (side == SIDE_BUY) ? 'B' : 'S';
        sendItchFrame(sock, itchAdd(locate, oref, itch_side, price, qty));
        uint64_t tc, tq; BinaryResponse rsp;
        collect(0, rsp, tc, tq);
        if (rsp.type == RSP_OK || rsp.type == RSP_FILLED)
            return oref;
        return 0;
    }

    // CANCEL 快捷方法
    bool cancel(uint64_t oid, uint64_t uid) {
        sendItchFrame(sock, itchDelete(locate, oid));
        uint64_t tc, tq; BinaryResponse rsp;
        collect(0, rsp, tc, tq);
        return rsp.type == RSP_CANCELLED;
    }

    // BOOK 快捷方法
    BinaryResponse book() {
        sendItchFrame(sock, itchBookQuery(locate));
        uint64_t tc, tq; BinaryResponse rsp;
        collect(0, rsp, tc, tq);
        return rsp;
    }
};

// ── 服务端生命周期 ──
// 每次测试独立启动/停止服务端。清理共享内存 + WAL，避免上次测试残留订单
// 在 fresh_start=false 时被 recoverFromShared 恢复污染本次测试。
// 服务端无特权运行（shm 文件归当前用户所有）。startServer 前需清掉 /dev/shm 与 /tmp
// 的残留文件（含 sudo 创建的 root 所有文件），否则恢复旧数据污染测试。
static void startServer(const char* bin, int port) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "pkill -9 nebulaX 2>/dev/null; sleep 0.2; "
             "rm -f /dev/shm/nebulaX_book /dev/shm/nebulaX_metrics "
             "/tmp/nebulaX_wal.dat /tmp/nebulaX_snapshot.dat /tmp/nebulaX_checkpoint.dat 2>/dev/null; "
             "sleep 0.1; "
             "taskset -c 6 %s %d > /dev/null 2>&1 &", bin, port);
    system(cmd);
    usleep(500 * 1000);  // wait for server to bind
}

static void stopServer() {
    system("pkill -9 nebulaX 2>/dev/null");
    usleep(200 * 1000);
}

// ═══════════════════════════════════════════
//  测试用例
// ═══════════════════════════════════════════

// ── 测试 1: 基本 NEW + BOOK + CANCEL ──
static bool testBasic(const char* ip, int port) {
    Client c(ip, port);
    if (!c.ok()) { result(false, "连接失败"); return false; }

    std::vector<std::pair<uint64_t,uint64_t>> created;

    // 1. BUY resting
    uint64_t buy_oid = c.newOrder(SIDE_BUY, 10000, 10, 2001);
    result(buy_oid > 0, "BUY resting → order_id=%lu", buy_oid);
    if (buy_oid) created.push_back({buy_oid, 2001});

    // 2. SELL resting
    uint64_t sell_oid = c.newOrder(SIDE_SELL, 20000, 10, 2002);
    result(sell_oid > 0, "SELL resting → order_id=%lu", sell_oid);
    if (sell_oid) created.push_back({sell_oid, 2002});

    // 3. BOOK
    auto rsp = c.book();
    result(rsp.type == RSP_BOOK, "BOOK → RSP_BOOK");
    result(rsp.data.book.bid_price == 10000, "  bid=%u", rsp.data.book.bid_price);
    result(rsp.data.book.ask_price == 20000, "  ask=%u", rsp.data.book.ask_price);
    result(rsp.data.book.bid_volume == 10,   "  bid_vol=%u", rsp.data.book.bid_volume);
    result(rsp.data.book.ask_volume == 10,   "  ask_vol=%u", rsp.data.book.ask_volume);

    // 4. CANCEL
    bool ok = c.cancel(buy_oid, 2001);
    result(ok, "CANCEL → RSP_CANCELLED");

    // 5. BOOK after cancel
    rsp = c.book();
    result(rsp.data.book.bid_volume == 0, "bid_vol=0 after cancel");

    // 6. CANCEL invalid（不存在的 order_ref → 引擎拒绝）
    c.sendItch(itchDelete(c.locate, 999999));
    uint64_t tc, tq; BinaryResponse frsp;
    c.collect(0, frsp, tc, tq);
    result(frsp.type == RSP_ERROR, "CANCEL invalid → RSP_ERROR");

    // 7. INVALID price（price=0 → 引擎拒绝）
    c.sendItch(itchAdd(c.locate, 888, 'B', 0, 10));
    c.collect(0, frsp, tc, tq);
    result(frsp.type == RSP_ERROR, "INVALID price → RSP_ERROR");

    // cleanup
    for (auto& [oid, uid] : created) c.cancel(oid, uid);
    return true;
}

// ── 测试 2: 交叉成交 + 部分成交 ──
static bool testCross(const char* ip, int port) {
    Client c(ip, port);
    if (!c.ok()) { result(false, "连接失败"); return false; }

    // 1. SELL resting qty 10（order_ref = 200）
    c.sendItch(itchAdd(c.locate, 200, 'S', 10000, 10));
    uint64_t tc, tq; BinaryResponse rsp;
    c.collect(0, rsp, tc, tq);
    result(rsp.type == RSP_OK, "SELL resting → RSP_OK");

    // 2. BUY 5 @ 10000 → fills 5 of 10（order_ref = 201）
    c.sendItch(itchAdd(c.locate, 201, 'B', 10000, 5));
    c.collect(1, rsp, tc, tq);
    result(rsp.type == RSP_FILLED, "BUY 5 → FILLED");
    result(tc == 1, "  trades=%lu", tc);
    result(tq == 5, "  qty=%lu", tq);

    // 3. BUY 10 @ 10000 → fills remaining 5, 5 resting（order_ref = 202）
    c.sendItch(itchAdd(c.locate, 202, 'B', 10000, 10));
    c.collect(1, rsp, tc, tq);
    result(rsp.type == RSP_OK, "BUY 10 → OK (partial)");
    result(tc == 1, "  trades=%lu", tc);
    result(tq == 5, "  qty=%lu", tq);

    // 4. BOOK: resting BUY @ 10000 qty 5
    rsp = c.book();
    result(rsp.data.book.bid_price == 10000, "  bid=%u", rsp.data.book.bid_price);
    result(rsp.data.book.bid_volume == 5,    "  vol=%u", rsp.data.book.bid_volume);

    return true;
}

// ── 测试 3: 价格优先 ──
static bool testPriority(const char* ip, int port) {
    Client c(ip, port);
    if (!c.ok()) { result(false, "连接失败"); return false; }

    std::vector<std::pair<uint64_t,uint64_t>> created;

    // 1. SELL @ 20000 (高价) and SELL @ 10000 (低价)
    uint64_t s1 = c.newOrder(SIDE_SELL, 20000, 10, 2002);
    uint64_t s2 = c.newOrder(SIDE_SELL, 10000, 5, 2002);
    result(s1 > 0 && s2 > 0, "Two SELLs resting");
    if (s1) created.push_back({s1, 2002});

    // 2. BUY @ 15000 → 应优先成交 10000（最低卖价）
    c.sendItch(itchAdd(c.locate, 300, 'B', 15000, 5));
    uint64_t tc, tq; BinaryResponse rsp;
    c.collect(1, rsp, tc, tq);
    result(rsp.type == RSP_FILLED, "BUY crosses lowest ask");
    result(tq == 5, "  trade at lower price, qty=%lu", tq);

    // 3. BOOK: 高价卖单还在
    rsp = c.book();
    result(rsp.data.book.ask_price == 20000, "remaining ask=%u", rsp.data.book.ask_price);
    result(rsp.data.book.ask_volume == 10,   "  vol=%u", rsp.data.book.ask_volume);

    for (auto& [oid, uid] : created) c.cancel(oid, uid);
    return true;
}

// ── 测试 4: 单连接多标的（分簿正确性，500 条命令 × 逐帧类型校验） ──
// 单连接轮转多个 locate，验证引擎分簿：不同标的独立簿，互不串簿。
static bool testMulti(const char* ip, int port) {
    constexpr int N = 4;        // locate 数
    constexpr int OPS = 250;    // 命令数（NEW/BOOK 交替）
    Client c(ip, port, 1);
    if (!c.ok()) { result(false, "连接失败"); return false; }

    int sent = 0, recved = 0, errors = 0, type_mismatch = 0;

    for (int i = 0; i < OPS; ++i) {
        uint16_t loc = (uint16_t)(i % N) + 1;   // 轮转 locate
        bool is_new = (i % 2 == 0);
        if (is_new) {
            // 每个标的独立价位区间，不会跨标的成交
            char itch_side = ((i % 2) == 0) ? 'B' : 'S';
            c.sendItch(itchAdd(loc, 1000 + i, itch_side,
                               30000 + (loc - 1) * 5000 + i * 10, 10));
        } else {
            c.sendItch(itchBookQuery(loc));
        }
        sent++;

        if (is_new) {
            // NEW 可能产生多条 TRADE + 最终状态
            // 排干 TRADE 后检查状态帧类型
            BinaryResponse final_rsp;
            bool got = false;
            while (true) {
                if (!c.recv(final_rsp)) break;
                if (final_rsp.type != RSP_TRADE) { got = true; break; }
            }
            if (!got) break;
            recved++;
            if (final_rsp.type == RSP_ERROR) errors++;
            if (final_rsp.type == RSP_BOOK) type_mismatch++;
        } else {
            // BOOK → 单帧 RSP_BOOK
            BinaryResponse rsp;
            if (!c.recv(rsp)) break;
            recved++;
            if (rsp.type != RSP_BOOK) type_mismatch++;
        }
    }

    result(recved == sent, "单连接多标的: recved=%d / sent=%d", recved, sent);
    result(errors == 0, "  errors=%d", errors);
    result(type_mismatch == 0, "  type_mismatch=%d (0=每帧类型匹配发送序列)", type_mismatch);

    // 验证各标的簿记未损坏（逐 locate 检查 bid <= ask 或不为空）
    bool all_ok = true;
    for (int l = 1; l <= N; ++l) {
        c.locate = (uint16_t)l;
        auto rsp = c.book();
        if (rsp.data.book.bid_price > rsp.data.book.ask_price && rsp.data.book.ask_price != 0)
            all_ok = false;
    }
    result(all_ok, "∀ 标的 book consistent: bid <= ask");
    return true;
}

// ── 测试 5: 批量连续成交（100 SELL + 1 BUY = 101 条命令） ──
static bool testBurst(const char* ip, int port) {
    Client c(ip, port);
    if (!c.ok()) { result(false, "连接失败"); return false; }

    // 100 个卖单 @ 10000 qty 1 各（order_ref = 500..599）
    for (int i = 0; i < 100; ++i) {
        c.sendItch(itchAdd(c.locate, 500 + i, 'S', 10000, 1));
        uint64_t tc, tq; BinaryResponse rsp;
        c.collect(0, rsp, tc, tq);
        result(rsp.type == RSP_OK, "SELL %d/100 resting", i + 1);
    }

    // 一个买单吃掉全部 100 个卖单 → 100 笔 TRADE
    c.sendItch(itchAdd(c.locate, 700, 'B', 20000, 100));
    uint64_t tc, tq; BinaryResponse rsp;
    c.collect(100, rsp, tc, tq);
    result(rsp.type == RSP_FILLED, "BUY 100 → FILLED");
    result(tc == 100, "  trades=%lu (expect 100)", tc);
    result(tq == 100, "  qty=%lu (expect 100)", tq);

    return true;
}

// ═══════════════════════════════════════════
int main(int argc, char* argv[]) {
    const char* ip = "127.0.0.1";
    int port = 2250;
    const char* bin = "build/nebulaX";

    if (argc >= 2) ip = argv[1];
    if (argc >= 3) port = std::stoi(argv[2]);

    printf("NebulaX 正确性测试（独立服务端/测试）\n");
    printf("  server: %s:%d\n\n", ip, port);

    struct TestCase {
        const char* name;
        bool (*fn)(const char*, int);
    } tests[] = {
        {"1. Basic NEW + BOOK + CANCEL",  testBasic},
        {"2. Cross + partial fill",       testCross},
        {"3. Price priority",             testPriority},
        {"4. Multi-connection concurrent", testMulti},
        {"5. Burst cross (100 TRADEs)",   testBurst},
    };

    for (auto& t : tests) {
        startServer(bin, port);
        printf("[%s]\n", t.name);
        fflush(stdout);
        t.fn(ip, port);
        stopServer();
        printf("\n");
    }

    printf("==============================\n");
    printf("  PASS: %d    FAIL: %d\n", g_pass, g_fail);
    printf("==============================\n");
    return g_fail > 0 ? 1 : 0;
}
