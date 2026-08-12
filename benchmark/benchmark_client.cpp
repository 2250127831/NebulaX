// NebulaX ITCH 回放客户端（迁移自 NebulaX-Trader benchmark/main.cpp）
// 用法: ./benchmark_client <itch_file> [port]
//
// 只做正确性回放：读 ITCH 二进制文件 → 按 2B 长度前缀切消息 → 过滤出订单消息
// (A/F/D/X/U) → TCP 发送给撮合引擎 → 收响应 → 统计完成率。
// 成交消息(P/E/C)是撮合引擎的输出，不作为输入发送。
//
// 输出：发送订单数 / 收到确认数 / 成交回报数 / 完成率。

#include "../include/itch_parser.h"
#include "../include/protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <string>

#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

using namespace std::chrono;

// ── 简易 TCP 客户端 ──
class Client {
    int sock_ = -1;
    std::vector<char> read_buf_;

    void fillBuffer(size_t need) {
        while (read_buf_.size() < need) {
            char raw[8192];
            ssize_t n = ::recv(sock_, raw, sizeof(raw), 0);
            if (n <= 0) return;
            read_buf_.insert(read_buf_.end(), raw, raw + n);
        }
    }

public:
    Client(const char* ip, int port) {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip, &addr.sin_addr);
        if (connect(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock_); sock_ = -1;
        }
    }
    ~Client() { if (sock_ >= 0) close(sock_); }
    bool ok() const { return sock_ >= 0; }

    void sendFrame(const void* data, size_t len) {
        ::send(sock_, data, len, 0);
    }

    // 读下一帧（48B 响应）。返回 false = 连接关闭/错误。
    bool recvFrame(BinaryResponse& rsp) {
        fillBuffer(sizeof(rsp));
        if (read_buf_.size() < sizeof(rsp)) return false;
        memcpy(&rsp, read_buf_.data(), sizeof(rsp));
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + sizeof(rsp));
        return true;
    }

    // 读一条订单消息的完整响应：排干 RSP_TRADE，返回最终状态帧。
    // returns: true=读到最终帧, false=连接关闭
    bool recvFinal(BinaryResponse& final_rsp) {
        while (true) {
            BinaryResponse rsp;
            if (!recvFrame(rsp)) return false;
            if (rsp.type != RSP_TRADE) { final_rsp = rsp; return true; }
        }
    }
};

// ── 统计 ──
struct Stats {
    uint64_t sent = 0;          // 发送订单消息数
    uint64_t confirmed = 0;     // 收到最终状态帧数
    uint64_t trades = 0;        // 成交回报数（TRADE 帧）
    uint64_t errors = 0;        // RSP_ERROR 数
    uint64_t rejected = 0;      // 引擎拒绝（无效 order_ref 等）
    uint64_t err_not_found = 0;   // ORDER_NOT_FOUND
    uint64_t err_invalid = 0;     // INVALID_PRICE_QTY_USER
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <itch_file> [port]\n", argv[0]);
        return 1;
    }
    const char* file = argv[1];
    int port = (argc >= 3) ? std::stoi(argv[2]) : 2250;

    // ── mmap ITCH 文件 ──
    int fd = open(file, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    off_t file_size = lseek(fd, 0, SEEK_END);
    auto* buf = static_cast<uint8_t*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (buf == MAP_FAILED) { perror("mmap"); return 1; }

    // ── 连接撮合引擎 ──
    Client c("127.0.0.1", port);
    if (!c.ok()) { fprintf(stderr, "connect failed\n"); return 1; }

    ItchParser parser;
    Stats st;

    auto t0 = steady_clock::now();

    // ── 顺序解析 ITCH，发送订单消息 ──
    size_t pos = 0;
    uint64_t extra = 0;
    while (pos + 2 <= static_cast<size_t>(file_size)) {
        // ITCH 5.0: 2 字节 big-endian 长度前缀 = 消息体长度（含 type）
        uint16_t body_len = static_cast<uint16_t>((buf[pos] << 8) | buf[pos + 1]);
        if (body_len < 1 || body_len > 4096) {
            ++pos; ++extra;   // 损坏前缀，跳过对齐
            continue;
        }
        size_t msg_len = 2 + static_cast<size_t>(body_len);
        if (pos + msg_len > static_cast<size_t>(file_size)) break;

        // 只发订单消息（A/F/D/X/U）。用 ItchParser 判断类型。
        ItchEvent ev;
        if (parser.feed(buf + pos + 2, body_len, ev)) {
            // 发送完整帧（2B 前缀 + body）
            c.sendFrame(buf + pos, msg_len);
            st.sent++;

            // 收该订单的最终状态帧
            BinaryResponse final_rsp;
            if (!c.recvFinal(final_rsp)) {
                fprintf(stderr, "recv failed at order %lu\n", (unsigned long)st.sent);
                break;
            }
            st.confirmed++;
            if (final_rsp.type == RSP_TRADE) st.trades++;       // 不可能，但防御
            else if (final_rsp.type == RSP_ERROR) {
                st.errors++;
                if (final_rsp.data.error.code == static_cast<uint16_t>(ErrorCode::ORDER_NOT_FOUND))
                    st.err_not_found++;
                else if (final_rsp.data.error.code == static_cast<uint16_t>(ErrorCode::INVALID_PRICE_QTY_USER))
                    st.err_invalid++;
            }
        }
        pos += msg_len;
    }

    auto t1 = steady_clock::now();
    double sec = duration_cast<duration<double>>(t1 - t0).count();

    // ── 汇总 ──
    printf("\n===== ITCH 回放正确性验证 =====\n");
    printf("  订单消息发送:  %lu\n", (unsigned long)st.sent);
    printf("  收到确认:      %lu\n", (unsigned long)st.confirmed);
    printf("  错误:          %lu (NOT_FOUND=%lu, INVALID=%lu)\n",
           (unsigned long)st.errors, (unsigned long)st.err_not_found,
           (unsigned long)st.err_invalid);
    printf("  完成率:        %.1f%%\n", st.sent > 0 ? 100.0 * st.confirmed / st.sent : 0.0);
    printf("  耗时:          %.3f s\n", sec);
    printf("  QPS(参考):     %.0f\n", sec > 0 ? st.sent / sec : 0.0);

    bool ok = (st.confirmed == st.sent && st.errors == 0);
    printf("  %s\n", ok ? "✅ 全部订单确认，无错误" : "❌ 有未确认/错误");

    munmap(buf, file_size);
    return ok ? 0 : 1;
}
