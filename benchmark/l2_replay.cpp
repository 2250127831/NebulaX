// L2 逐笔数据 C++ 回放 — 加载 CSV → 多连接全速发单
// 编译: g++ -std=c++17 -O2 -I../include l2_replay.cpp -o l2_replay -lpthread
#include "../include/protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <csignal>

using namespace std::chrono;

static constexpr int N_CONN      = 4;
static constexpr int ORDERS_PER  = 20000;  // 每个连接 2 万笔
static constexpr int BATCH       = 4000;
static constexpr int PORT        = 2250;

struct PriceRec { int side; int price; int qty; };

// 加载 CSV
std::vector<PriceRec> load_csv(const char* path) {
    std::vector<PriceRec> out;
    FILE* f = fopen(path, "r");
    if (!f) { perror("fopen"); exit(1); }
    char buf[256];
    fgets(buf, sizeof(buf), f);  // skip header
    while (fgets(buf, sizeof(buf), f)) {
        PriceRec r;
        char typ[16];
        if (sscanf(buf, "%*d,%[^,],%d,%d,%d,%*d", typ, &r.side, &r.price, &r.qty) >= 4) {
            out.push_back(r);
        }
    }
    fclose(f);
    printf("  CSV: %zu records\n", out.size());
    return out;
}

struct Result { int ok; };

void worker(const std::vector<PriceRec>& prices, Result* res) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(PORT);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return; }

    int total_ok = 0;
    int off = 0;

    while (off < ORDERS_PER) {
        int n = std::min(BATCH, ORDERS_PER - off);
        char buf[BATCH * 32];
        char* p = buf;

        for (int i = 0; i < n && i < (int)prices.size(); i++) {
            auto& pr = prices[(off + i) % prices.size()];
            BinaryCommand cmd{};
            cmd.type = 0x01;  // NEW
            cmd.side = (pr.side == 1) ? 0x01 : 0x02;
            cmd.price = pr.price;
            cmd.quantity = pr.qty ? pr.qty : 100;
            cmd.user_id = off + i + 1;
            memcpy(p, &cmd, sizeof(cmd));
            p += sizeof(cmd);
        }

        ssize_t sent = ::send(sock, buf, p - buf, 0);
        if (sent <= 0) break;

        // recv responses
        int need = n * 48;
        while (need > 0) {
            char rbuf[65536];
            ssize_t r = ::recv(sock, rbuf, std::min(need, 65536), 0);
            if (r <= 0) { need = 0; break; }
            need -= r;
        }
        if (need != 0) break;
        total_ok += n;
        off += n;
    }

    res->ok = total_ok;
    close(sock);
}

int main() {
    setbuf(stdout, nullptr);
    signal(SIGPIPE, SIG_IGN);
    printf("===== L2 C++ Replay =====\n");
    auto prices = load_csv("/tmp/l2_replay.csv");
    printf("  loaded %zu prices\n", prices.size());

    std::thread thr[N_CONN];
    Result res[N_CONN];

    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < N_CONN; i++)
        thr[i] = std::thread(worker, std::ref(prices), &res[i]);
    for (int i = 0; i < N_CONN; i++)
        thr[i].join();
    auto t1 = high_resolution_clock::now();

    int total = 0;
    for (int i = 0; i < N_CONN; i++) total += res[i].ok;
    double sec = duration_cast<duration<double>>(t1 - t0).count();
    printf("Result: %d orders  %.2fs  %.0f QPS\n", total, sec, total / sec);
}
