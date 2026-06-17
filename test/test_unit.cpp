// NebulaX 单元测试 —— MPSCRing、OrderPool、WalEntry 等核心组件
#include <cstdio>
#include <cstring>
#include <cassert>
#include "../include/mpsc_ring.h"
#include "../include/order_pool.h"
#include "../include/wal.h"

static int passed = 0, failed = 0;
#define TEST(name) do { printf("  %s ... ", name); } while(0)
#define OK() do { printf("OK\n"); passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); failed++; } while(0)

void test_mpsc_ring() {
    TEST("single producer / consumer");
    MPSCRing<int, 64> ring;
    int handle = ring.alloc();
    *ring.handleData(handle) = 42;
    ring.commit(handle);

    auto* p = ring.tryPop();
    assert(p && *p == 42);
    ring.consume();
    assert(ring.tryPop() == nullptr);  // empty
    OK();
}

void test_mpsc_ring_wrap() {
    TEST("wrap around + overwrite");
    MPSCRing<int, 4> ring;
    // fill all slots
    int h[5];
    for (int i = 0; i < 4; i++) {
        h[i] = ring.alloc();
        *ring.handleData(h[i]) = i;
        ring.commit(h[i]);
    }
    // consume 2
    for (int i = 0; i < 2; i++) {
        auto* p = ring.tryPop();
        assert(p && *p == i);
        ring.consume();
    }
    // wrap: alloc 2 more (reuses slots 0,1)
    for (int i = 4; i < 6; i++) {
        h[i] = ring.alloc();
        *ring.handleData(h[i]) = i;
        ring.commit(h[i]);
    }
    // read remaining in order: 2,3,4,5
    for (int i = 2; i < 6; i++) {
        auto* p = ring.tryPop();
        assert(p && *p == i);
        ring.consume();
    }
    assert(ring.tryPop() == nullptr);
    OK();
}

void test_order_pool() {
    TEST("OrderPool alloc / dealloc / rebuild");
    OrderPool pool(1024);
    Order* o = pool.allocate();
    assert(o != nullptr);
    o->order_id = 1;
    assert(pool.size() == 1);

    pool.deallocate(o);
    assert(pool.size() == 0);
    OK();
}

void test_order_pool_rebuild() {
    TEST("OrderPool rebuildFreelist");
    // 用外部存储模拟共享内存恢复
    Order storage[1024]{};
    // 模拟 3 个活跃订单在 storage[10], [20], [30]
    storage[10].order_id = 100;  storage[10].status = OrderStatus::OPEN;
    storage[20].order_id = 200;  storage[20].status = OrderStatus::OPEN;
    storage[30].order_id = 300;  storage[30].status = OrderStatus::OPEN;

    OrderPool pool(storage, 1024, false);  // 不初始化 free list
    pool.rebuildFreelist();                // 重建（order_id==0 的加入 free list）

    // allocate 应该避开 10,20,30
    for (int i = 0; i < 10; i++) {
        Order* o = pool.allocate();
        assert(o != nullptr);
        uint32_t idx = pool.indexOf(o);
        assert(idx != 10 && idx != 20 && idx != 30);
    }
    // 此时 10,20,30 仍然有原始数据
    assert(storage[10].order_id == 100);
    assert(storage[20].order_id == 200);
    assert(storage[30].order_id == 300);
    assert(pool.size() == 10);
    OK();
}

void test_wal_entry() {
    TEST("WalEntry size");
    assert(sizeof(WalEntry) == 40);
    OK();
}

int main() {
    printf("NebulaX unit tests\n==================\n\n");

    test_mpsc_ring();
    test_mpsc_ring_wrap();
    test_order_pool();
    test_order_pool_rebuild();
    test_wal_entry();

    printf("\n==================\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
