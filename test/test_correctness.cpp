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

// ── 简易 TCP 客户端 ──
struct Client {
    int sock = -1;
    std::vector<char> buf;

    Client(const char* ip, int port) {
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

    void send(const BinaryCommand& cmd) {
        ::send(sock, &cmd, sizeof(cmd), 0);
    }

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
        BinaryCommand cmd{};
        cmd.type = CMD_NEW; cmd.side = side;
        cmd.price = price; cmd.quantity = qty; cmd.user_id = uid;
        send(cmd);
        uint64_t tc, tq; BinaryResponse rsp;
        collect(0, rsp, tc, tq);
        if (rsp.type == RSP_OK || rsp.type == RSP_FILLED)
            return rsp.data.ack.order_id;
        return 0;
    }

    // CANCEL 快捷方法
    bool cancel(uint64_t oid, uint64_t uid) {
        BinaryCommand cmd{};
        cmd.type = CMD_CANCEL; cmd.order_id = oid; cmd.user_id = uid;
        send(cmd);
        uint64_t tc, tq; BinaryResponse rsp;
        collect(0, rsp, tc, tq);
        return rsp.type == RSP_CANCELLED;
    }

    // BOOK 快捷方法
    BinaryResponse book() {
        BinaryCommand cmd{}; cmd.type = CMD_BOOK;
        send(cmd);
        uint64_t tc, tq; BinaryResponse rsp;
        collect(0, rsp, tc, tq);
        return rsp;
    }
};

// ── 服务端生命周期 ──
static void startServer(const char* bin, int port) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pkill -9 nebulaX 2>/dev/null; sleep 0.2; "
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

    // 6. CANCEL invalid
    BinaryCommand cmd{}; cmd.type = CMD_CANCEL; cmd.order_id = 999999; cmd.user_id = 2001;
    c.send(cmd);
    uint64_t tc, tq; BinaryResponse frsp;
    c.collect(0, frsp, tc, tq);
    result(frsp.type == RSP_ERROR, "CANCEL invalid → RSP_ERROR");

    // 7. INVALID side
    cmd.type = CMD_NEW; cmd.side = 0xFF; cmd.price = 100; cmd.quantity = 10; cmd.user_id = 2001;
    c.send(cmd);
    c.collect(0, frsp, tc, tq);
    result(frsp.type == RSP_ERROR, "INVALID side → RSP_ERROR");

    // cleanup
    for (auto& [oid, uid] : created) c.cancel(oid, uid);
    return true;
}

// ── 测试 2: 交叉成交 + 部分成交 ──
static bool testCross(const char* ip, int port) {
    Client c(ip, port);
    if (!c.ok()) { result(false, "连接失败"); return false; }

    // 1. SELL resting qty 10
    BinaryCommand cmd{};
    cmd.type = CMD_NEW; cmd.side = SIDE_SELL; cmd.price = 10000; cmd.quantity = 10; cmd.user_id = 2002;
    c.send(cmd);
    uint64_t tc, tq; BinaryResponse rsp;
    c.collect(0, rsp, tc, tq);
    result(rsp.type == RSP_OK, "SELL resting → RSP_OK");

    // 2. BUY 5 @ 10000 → fills 5 of 10
    cmd.type = CMD_NEW; cmd.side = SIDE_BUY; cmd.price = 10000; cmd.quantity = 5; cmd.user_id = 2001;
    c.send(cmd);
    c.collect(1, rsp, tc, tq);
    result(rsp.type == RSP_FILLED, "BUY 5 → FILLED");
    result(tc == 1, "  trades=%lu", tc);
    result(tq == 5, "  qty=%lu", tq);

    // 3. BUY 10 @ 10000 → fills remaining 5, 5 resting
    cmd.type = CMD_NEW; cmd.side = SIDE_BUY; cmd.price = 10000; cmd.quantity = 10; cmd.user_id = 2001;
    c.send(cmd);
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
    BinaryCommand cmd{};
    cmd.type = CMD_NEW; cmd.side = SIDE_BUY; cmd.price = 15000; cmd.quantity = 5; cmd.user_id = 2001;
    c.send(cmd);
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

// ── 测试 4: 多连接并发（1000 条命令 × 逐帧类型校验） ──
static bool testMulti(const char* ip, int port) {
    constexpr int N = 4;
    constexpr int OPS = 250;  // 125 NEW + 125 BOOK  × 4 连接 = 1000 条
    struct ThreadResult {
        int sent = 0, recved = 0, errors = 0;
        int type_mismatch = 0;  // NEW→BOOK 或 BOOK→NEW
    };
    std::vector<ThreadResult> tr(N);

    std::vector<std::thread> threads;
    for (int ci = 0; ci < N; ++ci) {
        threads.emplace_back([&, ci]() {
            Client c(ip, port);
            if (!c.ok()) { tr[ci].errors = 999; return; }

            BinaryCommand cmd{};

            for (int i = 0; i < OPS; ++i) {
                memset(&cmd, 0, sizeof(cmd));
                bool is_new = (i % 2 == 0);
                if (is_new) {
                    cmd.type = CMD_NEW;
                    // 每条连接用自己的价位区间，不会跨连接成交
                    cmd.side = (ci + i) % 2 == 0 ? SIDE_BUY : SIDE_SELL;
                    cmd.price = 30000 + ci * 5000 + i * 10;
                    cmd.quantity = 10;
                    cmd.user_id = 10000 + ci * 100 + i;
                } else {
                    cmd.type = CMD_BOOK;
                }
                c.send(cmd);
                tr[ci].sent++;

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
                    tr[ci].recved++;
                    if (final_rsp.type == RSP_ERROR) tr[ci].errors++;
                    if (final_rsp.type == RSP_BOOK) tr[ci].type_mismatch++;
                } else {
                    // BOOK → 单帧 RSP_BOOK
                    BinaryResponse rsp;
                    if (!c.recv(rsp)) break;
                    tr[ci].recved++;
                    if (rsp.type != RSP_BOOK) tr[ci].type_mismatch++;
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    int total_sent = 0, total_recv = 0, total_err = 0, total_mm = 0;
    for (auto& r : tr) {
        total_sent += r.sent;
        total_recv += r.recved;
        total_err += r.errors;
        total_mm += r.type_mismatch;
    }
    result(total_recv == total_sent,
           "∀ 连接: recved=%d / sent=%d", total_recv, total_sent);
    result(total_err == 0, "  errors=%d", total_err);
    result(total_mm == 0, "  type_mismatch=%d (0=每帧类型匹配发送序列)", total_mm);

    // 验证簿记未损坏
    Client c(ip, port);
    if (c.ok()) {
        auto rsp = c.book();
        result(rsp.data.book.bid_price <= rsp.data.book.ask_price || rsp.data.book.ask_price == 0,
               "book consistent: bid=%u <= ask=%u",
               rsp.data.book.bid_price, rsp.data.book.ask_price);
    }
    return true;
}

// ── 测试 5: 批量连续成交（100 SELL + 1 BUY = 101 条命令） ──
static bool testBurst(const char* ip, int port) {
    Client c(ip, port);
    if (!c.ok()) { result(false, "连接失败"); return false; }

    // 100 个卖单 @ 10000 qty 1 各
    for (int i = 0; i < 100; ++i) {
        BinaryCommand cmd{};
        cmd.type = CMD_NEW; cmd.side = SIDE_SELL; cmd.price = 10000; cmd.quantity = 1; cmd.user_id = 2002;
        c.send(cmd);
        uint64_t tc, tq; BinaryResponse rsp;
        c.collect(0, rsp, tc, tq);
        result(rsp.type == RSP_OK, "SELL %d/100 resting", i + 1);
    }

    // 一个买单吃掉全部 100 个卖单 → 100 笔 TRADE
    BinaryCommand cmd{};
    cmd.type = CMD_NEW; cmd.side = SIDE_BUY; cmd.price = 20000; cmd.quantity = 100; cmd.user_id = 2001;
    c.send(cmd);
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
