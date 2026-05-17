// NebulaX benchmark client — pipeline mode
// Usage: ./benchmark_client <server_ip> [port]
// Pins itself to core 5 by default.
// Server on a different core via taskset:
//   taskset -c 0 ./nebulaX 2250
//   taskset -c 1 ./benchmark_client 127.0.0.1 2250

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <random>
#include <thread>
#include <atomic>

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
    std::string buf_;
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

    void sendLine(const std::string& line) {
        auto m = line + '\n';
        ::send(sock_, m.data(), m.size(), 0);
    }

    std::string recvLine() {
        std::string line;
        while (true) {
            auto pos = buf_.find('\n');
            if (pos != std::string::npos) {
                line = buf_.substr(0, pos);
                buf_.erase(0, pos + 1);
                return line;
            }
            char raw[4096];
            auto n = ::recv(sock_, raw, sizeof(raw), 0);
            if (n <= 0) { line = buf_; buf_.clear(); return line; }
            buf_.append(raw, n);
        }
    }

    std::string recvResponse() {
        std::string full;
        while (true) {
            auto line = recvLine();
            if (!full.empty()) full += '\n';
            full += line;
            if (line.substr(0,3)=="OK ") break;
            if (line.substr(0,6)=="ERROR ") break;
            if (line.substr(0,7)=="FILLED ") break;
            if (line.substr(0,10)=="CANCELLED ") break;
            if (line.find("BOOK_END") != std::string::npos) break;
        }
        return full;
    }
};

// ── helpers ──
struct OrderInfo {
    uint64_t order_id;
    uint64_t user_id;
};

static uint64_t parseOrderId(const std::string& resp) {
    auto eq = resp.find('=');
    if (eq != std::string::npos) return std::stoull(resp.substr(eq+1));
    auto sp = resp.rfind(' ');
    if (sp != std::string::npos) return std::stoull(resp.substr(sp+1));
    return 0;
}

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
        std::cerr << "Usage: " << argv[0] << " <server_ip> [port] [mode]\n";
        std::cerr << "  mode: -p (pipeline, default), -r (pingpong rtt)\n";
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
        if (!c.ok()) { std::cerr << "connect failed\n"; continue; }

        // ── pre-fill: 100 orders each side (ping-pong, not measured) ──
        std::vector<OrderInfo> open_orders;
        for (int i = 0; i < 100; ++i) {
            c.sendLine("NEW BUY "  + std::to_string(9000 + i % 100) + " 10 " + std::to_string(BUY_UID));
            auto r = c.recvResponse();
            if (r.find("OK order_id=") != std::string::npos)
                open_orders.push_back({parseOrderId(r), BUY_UID});
        }
        for (int i = 0; i < 100; ++i) {
            c.sendLine("NEW SELL " + std::to_string(25000 + i % 100) + " 10 " + std::to_string(SELL_UID));
            auto r = c.recvResponse();
            if (r.find("OK order_id=") != std::string::npos)
                open_orders.push_back({parseOrderId(r), SELL_UID});
        }

        if (pipeline) {
            // ── pipeline mode: 独立收发线程，测吞吐 ──
            SharedRing ring;
            uint64_t next_oid = 201;
            auto start_time = steady_clock::now();

            std::thread sender([&]() {
                for (auto& cmd : seq) {
                    auto idx = ring.send_idx.fetch_add(1, std::memory_order_relaxed);
                    ring.ts_send[idx] = __rdtsc();
                    if (cmd.type == 0) { // NEW
                        auto is_buy = (cmd.side == 0);
                        auto side_s = is_buy ? "BUY" : "SELL";
                        auto uid = is_buy ? BUY_UID : SELL_UID;
                        c.sendLine(std::string("NEW ") + side_s + " " + std::to_string(cmd.price)
                                   + " " + std::to_string(cmd.qty) + " " + std::to_string(uid));
                    } else {
                        c.sendLine("BOOK");  // CANCEL → BOOK
                    }
                }
            });

            std::thread receiver([&]() {
                uint64_t count = 0;
                while (count < TOTAL_CMDS) {
                    c.recvResponse();
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
                    auto is_buy = (cmd.side == 0);
                    auto side_s = is_buy ? "BUY" : "SELL";
                    auto uid = is_buy ? BUY_UID : SELL_UID;
                    c.sendLine(std::string("NEW ") + side_s + " " + std::to_string(cmd.price)
                               + " " + std::to_string(cmd.qty) + " " + std::to_string(uid));
                    sent = true;
                    auto resp = c.recvResponse();
                    if (resp.find("OK order_id=") != std::string::npos)
                        open_orders_pp.push_back({parseOrderId(resp), uid});
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
                        c.sendLine("CANCEL " + std::to_string(cancel_oid) + " " + std::to_string(cancel_uid));
                        sent = true;
                        c.recvResponse();
                        passed_cmds++;
                    } else {
                        c.sendLine("BOOK"); sent = true; c.recvResponse(); passed_cmds++;
                    }

                } else { // BOOK
                    c.sendLine("BOOK"); sent = true; c.recvResponse(); passed_cmds++;
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
