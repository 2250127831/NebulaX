// NebulaX benchmark client — pipeline / pingpong
// Usage: ./benchmark_client <server_ip> [port] [mode]
//   mode: -p (pipeline, default), -r (pingpong rtt)
// Pipeline 默认多连接（N_CONN=4），pingpong 始终单连接

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
#include <pthread.h>
#include <x86intrin.h>

using namespace std::chrono;

const uint64_t BUY_UID  = 1001;
const uint64_t SELL_UID = 1002;

// ── constants ──
constexpr int N_CONN       = 4;
constexpr int WARMUP_CMDS  = 10000;
constexpr int BATCH        = 128;

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

    void fillBuffer(size_t need) {
        while (read_buf_.size() < need) {
            char raw[4096];
            auto n = ::recv(sock_, raw, sizeof(raw), 0);
            if (n <= 0) return;
            read_buf_.insert(read_buf_.end(), raw, raw + n);
        }
    }

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

    void recvResponse(BinaryResponse& rsp) {
        do { readFrame(rsp); } while (rsp.type == RSP_TRADE);
    }
};

// ── helpers ──
struct Cmd {
    int type;     // 0=NEW, 1=CANCEL, 2=BOOK
    int side;     // 0=BUY, 1=SELL (NEW only)
    uint32_t price;
    uint32_t qty;
};

struct PerCmdStats { double us; int type; };
struct RunStats {
    double avg_us, p50, p99, p999, qps;
    int64_t total;
};

static double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    return v[(int)(p * v.size())];
}

// ── per-worker ring buffer for RDTSC timing ──
struct WorkerRing {
    uint64_t* ts_send;
    uint64_t* ts_recv;
    std::atomic<uint64_t> send_idx{0};
    std::atomic<uint64_t> recv_idx{0};

    explicit WorkerRing(size_t n) {
        ts_send = new uint64_t[n];
        ts_recv = new uint64_t[n];
    }
    ~WorkerRing() { delete[] ts_send; delete[] ts_recv; }
};

// ── worker: 一条连接上的 pipeline 收发 ──
struct WorkerResult {
    std::vector<double> lat_us;
    double qps;
    int64_t count;
    int64_t wall_us;
};

static WorkerResult runWorker(const char* ip, int port,
                               const Cmd* seq, int count,
                               bool do_prefill) {
    WorkerResult wr = {};

    Client c(ip, port);
    if (!c.ok()) { wr.count = 0; wr.qps = 0; return wr; }

    // pre-fill: 200 resting orders
    if (do_prefill) {
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
        }
    }

    // pipeline send/recv
    WorkerRing ring(count);
    auto start_time = steady_clock::now();

    std::thread sender([&]() {
        char batch_buf[BATCH * sizeof(BinaryCommand)];
        int batch_count = 0;

        auto flush = [&]() {
            if (batch_count > 0) {
                c.sendBatch(batch_buf, batch_count * sizeof(BinaryCommand));
                batch_count = 0;
            }
        };

        for (int i = 0; i < count; ++i) {
            auto idx = ring.send_idx.fetch_add(1, std::memory_order_relaxed);
            ring.ts_send[idx] = __rdtsc();

            auto* bc = reinterpret_cast<BinaryCommand*>(
                batch_buf + batch_count * sizeof(BinaryCommand));
            memset(bc, 0, sizeof(BinaryCommand));
            if (seq[i].type == 0) { // NEW
                bc->type = CMD_NEW;
                bc->side = (seq[i].side == 0) ? SIDE_BUY : SIDE_SELL;
                bc->price = seq[i].price;
                bc->quantity = seq[i].qty;
                bc->user_id = (seq[i].side == 0) ? BUY_UID : SELL_UID;
            } else {
                bc->type = CMD_BOOK;
            }
            batch_count++;
            if (batch_count >= BATCH) flush();
        }
        flush();
    });

    std::thread receiver([&]() {
        uint64_t received = 0;
        BinaryResponse rsp;
        while (received < (uint64_t)count) {
            c.recvResponse(rsp);
            auto idx = ring.recv_idx.fetch_add(1, std::memory_order_relaxed);
            ring.ts_recv[idx] = __rdtsc();
            received++;
        }
    });

    sender.join();
    receiver.join();

    auto wall_us = duration_cast<microseconds>(
        steady_clock::now() - start_time).count();

    wr.count = count;
    wr.wall_us = wall_us;
    wr.qps = count * 1'000'000.0 / wall_us;
    wr.lat_us.resize(count);
    double ns = tsc_ns();
    for (int i = 0; i < count; i++)
        wr.lat_us[i] = (ring.ts_recv[i] - ring.ts_send[i]) * ns / 1000.0;

    return wr;
}

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
    const int64_t TOTAL_CMDS = pipeline ? 50000000 : 1000000;
    const int N_NEW    = TOTAL_CMDS * 50 / 100;
    const int N_CANCEL = TOTAL_CMDS * 25 / 100;
    const int N_BOOK   = TOTAL_CMDS * 25 / 100;

    std::mt19937 rng(42);

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
        if (pipeline) {
            // ── pipeline mode: N_CONN 连接并行 ──
            // 预热
            {
                Client wc(ip.c_str(), port);
                if (!wc.ok()) { fprintf(stderr, "warmup connect failed\n"); continue; }
                // pre-fill 200
                for (int i = 0; i < 100; ++i) {
                    BinaryCommand cmd{}; cmd.type = CMD_NEW; cmd.side = SIDE_BUY;
                    cmd.price = 9000 + i % 100; cmd.quantity = 10; cmd.user_id = BUY_UID;
                    wc.sendCommand(cmd);
                    BinaryResponse rsp; wc.recvResponse(rsp);
                }
                for (int i = 0; i < 100; ++i) {
                    BinaryCommand cmd{}; cmd.type = CMD_NEW; cmd.side = SIDE_SELL;
                    cmd.price = 25000 + i % 100; cmd.quantity = 10; cmd.user_id = SELL_UID;
                    wc.sendCommand(cmd);
                    BinaryResponse rsp; wc.recvResponse(rsp);
                }
                // warmup commands
                for (int i = 0; i < WARMUP_CMDS; ++i) {
                    auto& c = seq[i % TOTAL_CMDS];
                    BinaryCommand bc{};
                    memset(&bc, 0, sizeof(bc));
                    if (c.type == 0) {
                        bc.type = CMD_NEW;
                        bc.side = (c.side == 0) ? SIDE_BUY : SIDE_SELL;
                        bc.price = c.price; bc.quantity = c.qty;
                        bc.user_id = (c.side == 0) ? BUY_UID : SELL_UID;
                    } else {
                        bc.type = CMD_BOOK;
                    }
                    wc.sendCommand(bc);
                    BinaryResponse rsp;
                    wc.recvResponse(rsp);
                }
            }

            // 多连接 pipeline 压测
            std::vector<WorkerResult> wrs(N_CONN);
            std::vector<std::thread> threads;
            int chunk = TOTAL_CMDS / N_CONN;

            // 第一个 worker 负责 pre-fill，后续 worker 不 pre-fill
            for (int i = 0; i < N_CONN; ++i) {
                int start = i * chunk;
                int cnt = (i == N_CONN - 1) ? TOTAL_CMDS - start : chunk;
                threads.emplace_back([&, i, start, cnt]() {
                    wrs[i] = runWorker(ip.c_str(), port,
                                       &seq[start], cnt,
                                       i == 0);  // only first does prefill
                });
            }
            for (auto& t : threads) t.join();

            // 汇总（QPS = 总命令数 / 最长耗时，避免多 worker 求和虚高）
            std::vector<double> all_lat;
            int64_t total_cmds = 0;
            int64_t max_wall_us = 0;
            for (auto& wr : wrs) {
                total_cmds += wr.count;
                if (wr.wall_us > max_wall_us) max_wall_us = wr.wall_us;
                all_lat.insert(all_lat.end(), wr.lat_us.begin(), wr.lat_us.end());
            }
            double true_qps = max_wall_us > 0 ? total_cmds * 1'000'000.0 / max_wall_us : 0;

            std::sort(all_lat.begin(), all_lat.end());
            double avg = 0;
            for (auto v : all_lat) avg += v;
            avg /= all_lat.size();
            double p50 = pct(all_lat, 0.50), p99 = pct(all_lat, 0.99), p999 = pct(all_lat, 0.999);

            printf("\nRun %d (pipeline, %d conns):\n", run + 1, N_CONN);
            printf("  QPS=%8.0f  avg=%5.0fus  P50=%4.0fus  P99=%5.0fus  P999=%5.0fus\n",
                   true_qps, avg, p50, p99, p999);
            runs.push_back({avg, p50, p99, p999, true_qps, total_cmds});

        } else {
            // ── pingpong mode: 单连接 RTT（保持不变） ──
            Client c(ip.c_str(), port);
            if (!c.ok()) { fprintf(stderr, "connect failed\n"); continue; }

            // pre-fill
            for (int i = 0; i < 100; ++i) {
                BinaryCommand cmd{};
                cmd.type = CMD_NEW; cmd.side = SIDE_BUY;
                cmd.price = 9000 + i % 100; cmd.quantity = 10; cmd.user_id = BUY_UID;
                c.sendCommand(cmd);
                BinaryResponse rsp; c.recvResponse(rsp);
            }
            for (int i = 0; i < 100; ++i) {
                BinaryCommand cmd{};
                cmd.type = CMD_NEW; cmd.side = SIDE_SELL;
                cmd.price = 25000 + i % 100; cmd.quantity = 10; cmd.user_id = SELL_UID;
                c.sendCommand(cmd);
                BinaryResponse rsp; c.recvResponse(rsp);
            }

            std::vector<PerCmdStats> cmd_stats;
            cmd_stats.reserve(TOTAL_CMDS);
            std::vector<uint64_t> open_oids;
            open_oids.reserve(200);

            for (auto& cmd : seq) {
                auto start = high_resolution_clock::now();
                bool sent = false;

                if (cmd.type == 0) { // NEW
                    BinaryCommand bc{};
                    bc.type = CMD_NEW;
                    bc.side = (cmd.side == 0) ? SIDE_BUY : SIDE_SELL;
                    bc.price = cmd.price; bc.quantity = cmd.qty;
                    bc.user_id = (cmd.side == 0) ? BUY_UID : SELL_UID;
                    c.sendCommand(bc); sent = true;
                    BinaryResponse rsp; c.recvResponse(rsp);
                    if (rsp.type == RSP_OK || rsp.type == RSP_FILLED)
                        open_oids.push_back(rsp.data.ack.order_id);

                } else if (cmd.type == 1 && !open_oids.empty()) { // CANCEL
                    uint64_t oid = open_oids.back(); open_oids.pop_back();
                    BinaryCommand bc{};
                    bc.type = CMD_CANCEL; bc.order_id = oid;
                    bc.user_id = (oid % 2 == 0) ? BUY_UID : SELL_UID;
                    c.sendCommand(bc); sent = true;
                    BinaryResponse rsp; c.recvResponse(rsp);

                } else { // BOOK
                    BinaryCommand bc{};
                    bc.type = CMD_BOOK;
                    c.sendCommand(bc); sent = true;
                    BinaryResponse rsp; c.recvResponse(rsp);
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
            printf("  QPS=%8.0f  avg=%5.0fus  P50=%4.0fus  P99=%5.0fus  P999=%5.0fus\n",
                   qps, avg, p50, p99, p999);
            runs.push_back({avg, p50, p99, p999, qps, (int)all_lat.size()});
        }
    }

    // ── summary ──
    printf("\n\n");
    printf("| Run | %6s | %5s | %5s | %5s | %5s | %8s |\n",
           "cmds", "avg", "P50", "P99", "P999", "QPS");
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
    printf("\nRuns: %d/%d successful (%s)\n", (int)runs.size(), N_RUNS,
           pipeline ? "pipeline" : "pingpong");

    return 0;
}
