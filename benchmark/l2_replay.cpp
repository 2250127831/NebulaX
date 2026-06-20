// L2 逐笔数据 C++ 回放 — benchmark_client 模式（独立收发线程）
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
#include <csignal>

using namespace std::chrono;

static constexpr int N_CONN     = 1;
static constexpr int ORDERS_PER = 80000;
static constexpr int PORT       = 2250;

struct PriceRec { int side; int price; int qty; };

std::vector<PriceRec> load_csv(const char* path) {
    std::vector<PriceRec> out;
    FILE* f = fopen(path, "r");
    if (!f) { perror("fopen"); exit(1); }
    char buf[256];
    fgets(buf, sizeof(buf), f);
    while (fgets(buf, sizeof(buf), f)) {
        PriceRec r; char typ[16];
        if (sscanf(buf, "%*d,%[^,],%d,%d,%d,%*d", typ, &r.side, &r.price, &r.qty) >= 4)
            out.push_back(r);
    }
    fclose(f);
    return out;
}

// 线程间原子计数器 + 同一 socket
struct SharedConn {
    int fd = -1;
    std::atomic<int> sent{0};
    std::atomic<int> recvd{0};
};

void sender(const std::vector<PriceRec>& prices, SharedConn* sc) {
    int off = 0;
    while (off < ORDERS_PER) {
        int n = std::min(128, ORDERS_PER - off);
        char buf[128 * 32];
        char* p = buf;
        for (int i = 0; i < n; i++) {
            auto& pr = prices[(off + i) % prices.size()];
            BinaryCommand cmd{};
            cmd.type = 0x01;
            cmd.side = (pr.side == 1) ? 0x01 : 0x02;
            cmd.price = pr.price;
            cmd.quantity = pr.qty ? pr.qty : 100;
            cmd.user_id = off + i + 1;
            memcpy(p, &cmd, sizeof(cmd)); p += sizeof(cmd);
        }
        if (::send(sc->fd, buf, p - buf, 0) <= 0) break;
        off += n;
        sc->sent.store(off, std::memory_order_release);
    }
}

void receiver(SharedConn* sc) {
    while (sc->recvd.load(std::memory_order_acquire) < ORDERS_PER) {
        char buf[65536];
        ssize_t r = ::recv(sc->fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        sc->recvd.fetch_add(r / 48, std::memory_order_release);
    }
}

int main() {
    setbuf(stdout, nullptr);
    signal(SIGPIPE, SIG_IGN);
    printf("===== L2 C++ Replay (独立收发) =====\n");
    auto prices = load_csv("/tmp/l2_replay.csv");
    printf("  CSV: %zu records, %d conns x %d orders\n", prices.size(), N_CONN, ORDERS_PER);

    std::thread sthr[N_CONN], rthr[N_CONN];
    SharedConn conns[N_CONN];
    auto t0 = high_resolution_clock::now();

    for (int i = 0; i < N_CONN; i++) {
        conns[i].fd = socket(AF_INET, SOCK_STREAM, 0);
        int rcvbuf = 4 * 1024 * 1024;  // 4MB recv buffer
        setsockopt(conns[i].fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(PORT);
        if (connect(conns[i].fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            printf("  connect %d failed\n", i);
            conns[i].fd = -1;
            continue;
        }
        sthr[i] = std::thread(sender, std::ref(prices), &conns[i]);
        rthr[i] = std::thread(receiver, &conns[i]);
    }

    for (int i = 0; i < N_CONN; i++) {
        if (conns[i].fd < 0) continue;
        sthr[i].join(); rthr[i].join();
        close(conns[i].fd);
    }

    auto t1 = high_resolution_clock::now();
    int total = 0;
    for (int i = 0; i < N_CONN; i++) {
        int s = conns[i].sent.load(), r = conns[i].recvd.load();
        printf("  conn%d: sent=%d recvd=%d\n", i, s, r);
        total += r;
    }
    double sec = duration_cast<duration<double>>(t1 - t0).count();
    printf("Result: %d orders  %.2fs  %.0f QPS\n", total, sec, total / sec);
}
