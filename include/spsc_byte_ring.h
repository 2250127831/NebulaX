#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>

// ── SPSCByteRing: 单生产者单消费者的字节环形缓冲区 ──
//
// push(data, len): 写入尽可能多的字节或部分字节后返回
// pop(buf, len):   读出尽可能多的字节或部分字节后返回
// read_acquire / read_release: 直接读取 ring 内部数据（零拷贝发送用）
// N 必须是 2 的幂。
template<size_t N>
class SPSCByteRing
{
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");

public:
    SPSCByteRing() : tail_(0), head_(0) {}

    // ── producer ──

    size_t push(const void* data, size_t len)
    {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t free = N - (tail - head);
        if (free == 0 || len == 0) return 0;

        size_t actual = (len < free) ? len : free;
        size_t mask   = N - 1;
        size_t pos    = tail & mask;

        size_t n1 = N - pos;
        if (n1 > actual) n1 = actual;
        if (n1 > 0) memcpy(buf_ + pos, data, n1);

        size_t n2 = actual - n1;
        if (n2 > 0)
            memcpy(buf_, static_cast<const uint8_t*>(data) + n1, n2);

        tail_.store(tail + actual, std::memory_order_release);
        return actual;
    }

    // ── consumer: 零拷贝读取接口 ──

    // 获取 ring 内部连续数据的指针。
    // 返回实际可用字节数（0 表示空），ptr 指向 ring 内部缓冲区。
    // 调用方读取后必须用 read_release 归还。
    size_t read_acquire(const void*& ptr, size_t request)
    {
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t head = head_.load(std::memory_order_relaxed);
        size_t used = tail - head;
        if (used == 0 || request == 0) { ptr = nullptr; return 0; }

        size_t actual = (request < used) ? request : used;
        size_t mask   = N - 1;
        size_t pos    = head & mask;
        size_t contig = N - pos;
        if (actual > contig) actual = contig;
        ptr = buf_ + pos;
        return actual;
    }

    // 归还已消费的字节。
    void read_release(size_t bytes)
    {
        head_.store(head_.load(std::memory_order_relaxed) + bytes,
                    std::memory_order_release);
    }

    // ── consumer: 带拷贝的完整读取（内部处理 wrap）──

    size_t pop(void* buf, size_t len)
    {
        const void* ptr;
        size_t n1 = read_acquire(ptr, len);
        if (n1 == 0) return 0;
        memcpy(buf, ptr, n1);
        read_release(n1);

        // 如果 len > n1 说明需要读第二段（wrap 后的数据）
        size_t remaining = len - n1;
        if (remaining > 0) {
            size_t n2 = read_acquire(ptr, remaining);
            if (n2 > 0) {
                memcpy(static_cast<uint8_t*>(buf) + n1, ptr, n2);
                read_release(n2);
                return n1 + n2;
            }
        }
        return n1;
    }

    // ── queries ──

    size_t free_space() const
    {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_relaxed);
        return N - (t - h);
    }

    // ring 缓冲区地址（用于 io_uring 注册为固定缓冲区）
    uint8_t* raw_buffer() { return buf_; }
    size_t   raw_size() const { return N; }

private:
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<size_t> head_{0};
    uint8_t buf_[N];
};
