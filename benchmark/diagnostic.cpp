// 诊断测试：隔离 "价格连续 vs 价格离散" 对连接的影响
#include "../include/protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <random>
using namespace std::chrono;

static constexpr int PORT = 2250;

int try_orders(int fd, int n, bool matching_prices) {
    for (int i = 0; i < n; i++) {
        BinaryCommand cmd{};
        cmd.type = 0x01;
        cmd.side = (i % 2) ? 0x01 : 0x02;          // alternating buy/sell
        if (matching_prices) {
            // 连续价格：buy 高价(100), sell 同价(100) → 必撮合
            cmd.price = 100;
        } else {
            // 离散价格：完全随机，大概率不撮合
            cmd.price = 100 + (rand() % 100000);
        }
        cmd.quantity = 100;
        cmd.user_id = i + 1;

        int r = ::send(fd, &cmd, sizeof(cmd), 0);
        if (r <= 0) return i;  // send failed

        BinaryResponse rsp;
        r = ::recv(fd, &rsp, sizeof(rsp), MSG_WAITALL);
        if (r <= 0) return i;  // recv failed
    }
    return n;
}

int test_connection(const char* label, int n_orders, bool matching) {
    printf("  [%s] connecting...\n", label);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("  [%s] socket failed\n", label); return -1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(PORT);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("  [%s] connect failed\n", label);
        close(fd);
        return -1;
    }

    int ok = try_orders(fd, n_orders, matching);
    printf("  [%s] %d/%d orders (%s)\n",
           label, ok, n_orders,
           ok == n_orders ? "PASS" : "FAIL");
    close(fd);
    // 等服务端释放资源
    usleep(200000);
    return ok;
}

int main() {
    setbuf(stdout, nullptr);
    signal(SIGPIPE, SIG_IGN);
    printf("===== NebulaX 连接诊断 =====\n\n");

    // 测试 1: 连续价格（必撮合）— 80K
    printf("--- Test 1: Continuous matching prices ---\n");
    int r1 = test_connection("matching", 80000, true);

    // 测试 2: 离散价格（大概率不撮合）— 80K
    printf("--- Test 2: Random non-matching prices ---\n");
    int r2 = test_connection("random", 80000, false);

    // 测试 3: 再测一次连续价格 → 看是否可复现
    printf("--- Test 3: Continuous prices again ---\n");
    int r3 = test_connection("matching2", 80000, true);

    printf("\n===== 结果汇总 =====\n");
    printf("  连续价格(1): %s (%d/80000)\n", r1 == 80000 ? "PASS" : "FAIL", r1);
    printf("  随机价格:    %s (%d/80000)\n", r2 >= 80000 ? "PASS" : "FAIL", r2);
    printf("  连续价格(2): %s (%d/80000)\n", r3 == 80000 ? "PASS" : "FAIL", r3);

    // 交叉验证：如果连续价格失败但随机成功 → 说明问题在撮合路径
    // 如果都失败 → 说明是 send/recv 路径或 ring 问题
    // 如果都成功 → 说明问题在 L2 数据具体特征
    return 0;
}
