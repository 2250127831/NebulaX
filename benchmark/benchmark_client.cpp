// NebulaX benchmark client — pipeline mode
// Usage: ./benchmark_client <server_ip> [port]
// Pins itself to core 5 by default.
// Server on a different core via taskset:
//   taskset -c 0 ./nebulaX 2250
//   taskset -c 1 ./benchmark_client 127.0.0.1 2250

#include "../include/protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <random>
#include <thread>
#include <atomic>
#include <vector>
#include <string>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sched.h>
#include <x86intrin.h>

using namespace std::chrono;

const uint64_t BUY_UID  = 1001;
const uint64_t SELL_UID = 1002;

// ── RDTSC calibration ──
static double tsc_ns() {
    static double ns = 0;
    if (ns == 0) {
        auto start = steady_clock::now();
        auto tsc = __rdtsc();
        while (steady_clock::now() - start < milliseconds(100));
        auto end_tsc = __rdtsc();
        auto end_t = steady_clock::now();
        ns = (double)duration_cast<nanoseconds>(end_t - start).count() / (end_tsc - tsc);
    }
    return ns;
}

// ── TCP Client ──
class Client {
    int sock_ = -1;
    std::vector<char> read_buf_;

    // Ensure at least need bytes available in the read buffer
    void fillBuffer(size_t need) {
        while (read_buf_.size() < need) {
            char raw[4096];
            auto n = ::recv(sock_, raw, sizeof(raw), 0);
            if (n <= 0) return;
            read_buf_.insert(read_buf_.end(), raw, raw + n);
        }
    }

    // Extract one 48-byte frame from buffer into rsp
    void readFrame(BinaryResponse& rsp) {
        fillBuffer(sizeof(rsp));
        memcpy(&rsp, read_buf_.data(), sizeof(rsp));
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + sizeof(rsp));
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

    void sendCommand(const BinaryCommand& cmd) {
        ::send(sock_, &cmd, sizeof(cmd), 0);
    }

    void sendBatch(const void* data, size_t len) {
        ::send(sock_, data, len, 0);
    }

    // Read one complete response, skipping any intermediate TRADE frames.
    // Uses an internal read buffer so one recv() feeds many frames.
    void recvResponse(BinaryResponse& rsp) {
        do {
            readFrame(rsp);
        } while (rsp.type == RSP_TRADE);
    }
};

// ── helpers ──
struct OrderInfo {
    uint64_t order_id;
    uint64_t user_id;
};

struct PerCmdStats { double us; int type; };
struct RunStats {
    double avg_us, p50, p99, p999, qps;
    int total;
};

static double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    return v[(int)(p * v.size())];
}

// ── shared ring buffer between send/recv threads ──
struct alignas(64) SharedRing {
    static constexpr size_t N = 4ul << 20;
    uint64_t* ts_send = new uint64_t[N];
    uint64_t* ts_recv = new uint64_t[N];
    std::atomic<uint64_t> send_idx{0};
    std::atomic<uint64_t> recv_idx{0};

    ~SharedRing() { delete[] ts_send; delete[] ts_recv; }
};

// ── main ──
int main(int argc, char* argv[]) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(5, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_ip> [port] [mode]\n", argv[0]);
        fprintf(stderr, "  mode: -p (pipeline, default), -r (pingpong rtt)\n");
        return 1;
    }
    std::string ip = argv[1];
    int port = (argc >= 3) ? std::stoi(argv[2]) : 2250;
    bool pipeline = true;
    if (argc >= 4 && std::string(argv[3]) == "-r") pipeline = false;

    tsc_ns();  // prime calibration

    const int N_RUNS   = 3;
    const int TOTAL_CMDS = 500000;
    const int N_NEW    = TOTAL_CMDS * 50 / 100;
    const int N_CANCEL = TOTAL_CMDS * 25 / 100;
    const int N_BOOK   = TOTAL_CMDS * 25 / 100;

    std::mt19937 rng(42);

    struct Cmd {
        int type;     // 0=NEW, 1=CANCEL, 2=BOOK
        int side;     // 0=BUY, 1=SELL (NEW only)
        uint32_t price;
        uint32_t qty;
    };
    std::vector<Cmd> seq(TOTAL_CMDS);
    {
        int idx = 0;
        for (int i = 0; i < N_NEW;   ++i) seq[idx++] = {0, 0, 0, 0};
        for (int i = 0; i < N_CANCEL;++i) seq[idx++] = {1, 0, 0, 0};
        for (int i = 0; i < N_BOOK;  ++i) seq[idx++] = {2, 0, 0, 0};
        std::shuffle(seq.begin(), seq.end(), rng);
        for (auto& c : seq) {
            if (c.type == 0) {
                c.side  = rng() % 2;
                c.price = 10000 + rng() % 5000;
                c.qty   = (rng() % 10 + 1) * 10;
            }
        }
    }

    std::vector<RunStats> runs;

    for (int run = 0; run < N_RUNS; ++run) {
        Client c(ip.c_str(), port);
        if (!c.ok()) { fprintf(stderr, "connect failed\n"); continue; }

        // ── pre-fill: 100 orders each side (ping-pong, not measured) ──
        std::vector<OrderInfo> open_orders;
        for (int i = 0; i < 100; ++i) {
            BinaryCommand cmd{};
            cmd.type = CMD_NEW;
            cmd.side = SIDE_BUY;
            cmd.price = 9000 + i % 100;
            cmd.quantity = 10;
            cmd.user_id = BUY_UID;
            c.sendCommand(cmd);

            BinaryResponse rsp;
            c.recvResponse(rsp);
            if (rsp.type == RSP_OK || rsp.type == RSP_FILLED)
                open_orders.push_back({rsp.data.ack.order_id, BUY_UID});
        }
        for (int i = 0; i < 100; ++i) {
            BinaryCommand cmd{};
            cmd.type = CMD_NEW;
            cmd.side = SIDE_SELL;
            cmd.price = 25000 + i % 100;
            cmd.quantity = 10;
            cmd.user_id = SELL_UID;
            c.sendCommand(cmd);

            BinaryResponse rsp;
            c.recvResponse(rsp);
            if (rsp.type == RSP_OK || rsp.type == RSP_FILLED)
                open_orders.push_back({rsp.data.ack.order_id, SELL_UID});
        }

        if (pipeline) {
            // ── pipeline mode: 独立收发线程，测吞吐 ──
            SharedRing ring;
            auto start_time = steady_clock::now();

            std::thread sender([&]() {
                // Batch sends: 128 × 32 bytes = 4096 bytes per flush
                constexpr int BATCH = 64;
                char batch_buf[BATCH * sizeof(BinaryCommand)];
                int batch_count = 0;

                auto flush = [&]() {
                    if (batch_count > 0) {
                        c.sendBatch(batch_buf,
                                    batch_count * sizeof(BinaryCommand));
                        batch_count = 0;
                    }
                };

                for (int si = 0; si < TOTAL_CMDS; ++si) {
                    auto& cmd = seq[si];
                    auto idx = ring.send_idx.fetch_add(1, std::memory_order_relaxed);
                    ring.ts_send[idx] = __rdtsc();

                    auto* bc = reinterpret_cast<BinaryCommand*>(
                        batch_buf + batch_count * sizeof(BinaryCommand));
                    memset(bc, 0, sizeof(BinaryCommand));
                    if (cmd.type == 0) { // NEW
                        bc->type = CMD_NEW;
                        bc->side = (cmd.side == 0) ? SIDE_BUY : SIDE_SELL;
                        bc->price = cmd.price;
                        bc->quantity = cmd.qty;
                        bc->user_id = (cmd.side == 0) ? BUY_UID : SELL_UID;
                    } else {
                        bc->type = CMD_BOOK;
                    }
                    batch_count++;

                    if (batch_count == BATCH)
                        flush();
                }
                flush(); // remaining
            });

            std::thread receiver([&]() {
                uint64_t count = 0;
                BinaryResponse rsp;
                while (count < TOTAL_CMDS) {
                    c.recvResponse(rsp);
                    auto idx = ring.recv_idx.fetch_add(1, std::memory_order_relaxed);
                    ring.ts_recv[idx] = __rdtsc();
                    count++;
                }
            });

            sender.join();
            receiver.join();

            auto wall_us = duration_cast<microseconds>(steady_clock::now() - start_time).count();
            double qps = TOTAL_CMDS * 1'000'000.0 / wall_us;
            double ns_per_tsc = tsc_ns();
            std::vector<double> lat_us(TOTAL_CMDS);
            for (uint64_t i = 0; i < TOTAL_CMDS; i++)
                lat_us[i] = (ring.ts_recv[i] - ring.ts_send[i]) * ns_per_tsc / 1000.0;
            std::sort(lat_us.begin(), lat_us.end());

            double avg = 0;
            for (auto v : lat_us) avg += v;
            avg /= TOTAL_CMDS;
            double p50 = pct(lat_us, 0.50), p99 = pct(lat_us, 0.99), p999 = pct(lat_us, 0.999);

            printf("\nRun %d (pipeline):\n", run + 1);
            printf("  QPS=%8.0f  avg=%5.0fus  P50=%4.0fus  P99=%5.0fus  P999=%5.0fus\n", qps, avg, p50, p99, p999);
            runs.push_back({avg, p50, p99, p999, qps, TOTAL_CMDS});

        } else {
            // ── pingpong mode: 发一条等一条，测 RTT ──
            std::vector<PerCmdStats> cmd_stats;
            cmd_stats.reserve(TOTAL_CMDS);
            int passed_cmds = 0;
            std::vector<OrderInfo> open_orders_pp = open_orders;

            for (auto& cmd : seq) {
                auto start = high_resolution_clock::now();
                bool sent = false;

                if (cmd.type == 0) { // NEW
                    BinaryCommand bc{};
                    bc.type = CMD_NEW;
                    bc.side = (cmd.side == 0) ? SIDE_BUY : SIDE_SELL;
                    bc.price = cmd.price;
                    bc.quantity = cmd.qty;
                    bc.user_id = (cmd.side == 0) ? BUY_UID : SELL_UID;
                    c.sendCommand(bc);
                    sent = true;

                    BinaryResponse rsp;
                    c.recvResponse(rsp);
                    if (rsp.type == RSP_OK || rsp.type == RSP_FILLED)
                        open_orders_pp.push_back({rsp.data.ack.order_id, bc.user_id});
                    passed_cmds++;

                } else if (cmd.type == 1) { // CANCEL
                    uint64_t cancel_oid = 0, cancel_uid = 0;
                    if (!open_orders_pp.empty()) {
                        auto pick = rng() % open_orders_pp.size();
                        cancel_oid = open_orders_pp[pick].order_id;
                        cancel_uid = open_orders_pp[pick].user_id;
                        open_orders_pp[pick] = open_orders_pp.back();
                        open_orders_pp.pop_back();
                    }
                    if (cancel_oid > 0) {
                        BinaryCommand bc{};
                        bc.type = CMD_CANCEL;
                        bc.order_id = cancel_oid;
                        bc.user_id = cancel_uid;
                        c.sendCommand(bc);
                        sent = true;

                        BinaryResponse rsp;
                        c.recvResponse(rsp);
                        passed_cmds++;
                    } else {
                        BinaryCommand bc{};
                        bc.type = CMD_BOOK;
                        c.sendCommand(bc);
                        sent = true;

                        BinaryResponse rsp;
                        c.recvResponse(rsp);
                        passed_cmds++;
                    }

                } else { // BOOK
                    BinaryCommand bc{};
                    bc.type = CMD_BOOK;
                    c.sendCommand(bc);
                    sent = true;

                    BinaryResponse rsp;
                    c.recvResponse(rsp);
                    passed_cmds++;
                }

                auto end = high_resolution_clock::now();
                if (sent) {
                    double us = duration_cast<microseconds>(end - start).count();
                    cmd_stats.push_back({us, cmd.type});
                }
            }

            std::vector<double> all_lat;
            all_lat.reserve(cmd_stats.size());
            for (auto& s : cmd_stats) all_lat.push_back(s.us);
            std::sort(all_lat.begin(), all_lat.end());

            double avg = 0;
            for (auto v : all_lat) avg += v;
            avg /= all_lat.size();
            double p50 = pct(all_lat, 0.50), p99 = pct(all_lat, 0.99), p999 = pct(all_lat, 0.999);
            double total_s = avg * all_lat.size() / 1'000'000.0;
            double qps = all_lat.size() / total_s;

            printf("\nRun %d (pingpong):\n", run + 1);
            printf("  QPS=%8.0f  avg=%5.0fus  P50=%4.0fus  P99=%5.0fus  P999=%5.0fus\n", qps, avg, p50, p99, p999);
            runs.push_back({avg, p50, p99, p999, qps, (int)all_lat.size()});
        }
    }

    // ── summary ──
    printf("\n\n");
    printf("| Run | %6s | %5s | %5s | %5s | %5s | %8s |\n", "cmds", "avg", "P50", "P99", "P999", "QPS");
    printf("|-----|--------|-------|-------|-------|-------|----------|\n");
    double sum_qps = 0, sum_avg = 0, sum_p50 = 0, sum_p99 = 0, sum_p999 = 0;
    for (int i = 0; i < (int)runs.size(); ++i) {
        auto& r = runs[i];
        printf("| %3d | %6d | %5.0f | %5.0f | %5.0f | %5.0f | %8.0f |\n",
               i + 1, r.total, r.avg_us, r.p50, r.p99, r.p999, r.qps);
        sum_qps += r.qps; sum_avg += r.avg_us;
        sum_p50 += r.p50; sum_p99 += r.p99; sum_p999 += r.p999;
    }
    int n = (int)runs.size();
    printf("| avg | %6d | %5.0f | %5.0f | %5.0f | %5.0f | %8.0f |\n",
           n ? runs[0].total : 0, sum_avg / n, sum_p50 / n,
           sum_p99 / n, sum_p999 / n, sum_qps / n);
    printf("\nRuns: %d/%d successful (%s)\n", (int)runs.size(), N_RUNS, pipeline ? "pipeline" : "pingpong");

    return 0;
}
