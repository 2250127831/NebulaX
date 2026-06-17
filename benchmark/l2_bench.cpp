// L2 压测 — 断连自动重连，100% 完成
#include "../include/protocol.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <random>
using namespace std::chrono;
static constexpr int TOTAL = 80000, PORT = 2250;
int main() {
    setbuf(stdout, nullptr); signal(SIGPIPE, SIG_IGN);
    int prices[100000], np = 0;
    FILE* f = fopen("/tmp/l2_replay.csv", "r"); if (!f) return 1;
    char b[256]; fgets(b, sizeof(b), f);
    while (fgets(b, sizeof(b), f) && np < 100000) {
        int p; if (sscanf(b, "%*d,%*[^,],%*d,%d,%*d,%*d", &p) >= 1) prices[np++] = p;
    }
    fclose(f);
    printf("prices: %d  orders: %d\n", np, TOTAL);

    int total_ok = 0;
    auto t0 = high_resolution_clock::now();
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> d(0, np - 1);

    while (total_ok < TOTAL) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(PORT);
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) break;

        int ok = 0;
        for (int i = total_ok; i < TOTAL; i++) {
            BinaryCommand cmd{};
            cmd.type = 0x01;
            cmd.side = (total_ok + ok) % 2 ? 0x01 : 0x02;
            cmd.price = cmd.side == 0x01 ? prices[d(rng)] : prices[d(rng)] + 1;
            cmd.quantity = 100; cmd.user_id = total_ok + ok + 1;
            int r = ::send(fd, &cmd, sizeof(cmd), 0);
            if (r <= 0) break;
            BinaryResponse rsp;
            r = ::recv(fd, &rsp, sizeof(rsp), MSG_WAITALL);
            if (r <= 0) break;
            ok++;
        }
        total_ok += ok;
        printf("  conn %d ok (total %d), reconnect in 1s...\n", ok, total_ok);
        close(fd);
        sleep(1);  // 等服务端释放 buffer
    }

    auto t = duration_cast<duration<double>>(high_resolution_clock::now() - t0).count();
    printf("Result: %d/%d  %.2fs  %.0f QPS\n", total_ok, TOTAL, t, total_ok / t);
}
