#include "NVMPI_bufPool.hpp"
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <iostream>
#include <cmath>

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    std::cout << "  " #fn "... " << std::flush; \
    fn(); \
    tests_passed++; \
    std::cout << "OK" << std::endl; \
} while(0)

template<typename F>
long elapsed_ms(F&& fn) {
    auto t0 = Clock::now();
    fn();
    auto t1 = Clock::now();
    return std::chrono::duration_cast<Ms>(t1 - t0).count();
}

static void test_dqFilledBuf_nonblocking_unchanged() {
    NVMPI_bufPool<int*> pool;
    auto ms = elapsed_ms([&]{ assert(pool.dqFilledBuf() == nullptr); });
    assert(ms < 50); (void)ms;
    int val = 42;
    pool.qFilledBuf(&val);
    int* out = pool.dqFilledBuf();
    assert(out == &val); (void)out;
    assert(pool.dqFilledBuf() == nullptr);
}

static void test_dqFilledBuf_blocking_returns_on_push() {
    NVMPI_bufPool<int*> pool;
    int val = 99;
    int* result = nullptr;
    std::thread consumer([&]{
        result = pool.dqFilledBuf(Ms(2000));
    });
    std::this_thread::sleep_for(Ms(100));
    pool.qFilledBuf(&val);
    consumer.join();
    assert(result == &val);
}

static void test_dqFilledBuf_blocking_returns_null_on_timeout() {
    NVMPI_bufPool<int*> pool;
    int* result = reinterpret_cast<int*>(1);
    auto ms = elapsed_ms([&]{
        result = pool.dqFilledBuf(Ms(200));
    });
    assert(result == nullptr);
    assert(ms >= 150 && ms <= 400); (void)ms;
}

static void test_dqFilledBuf_blocking_returns_null_on_shutdown() {
    NVMPI_bufPool<int*> pool;
    int* result = reinterpret_cast<int*>(1);
    std::thread consumer([&]{
        result = pool.dqFilledBuf(Ms(5000));
    });
    std::this_thread::sleep_for(Ms(100));
    pool.shutdown();
    consumer.join();
    assert(result == nullptr);
}

static void test_shutdown_wakes_all_waiters() {
    NVMPI_bufPool<int*> pool;
    constexpr int N = 3;
    std::atomic<int> woke{0};
    std::vector<std::thread> consumers;
    for (int i = 0; i < N; i++) {
        consumers.emplace_back([&]{
            pool.dqFilledBuf(Ms(5000));
            woke.fetch_add(1);
        });
    }
    std::this_thread::sleep_for(Ms(100));
    pool.shutdown();
    std::this_thread::sleep_for(Ms(200));
    assert(woke.load() == N);
    for (auto& t : consumers) t.join();
}

static void test_reset_clears_shutdown() {
    NVMPI_bufPool<int*> pool;
    int val = 77;
    pool.shutdown();
    pool.reset();
    int* result = nullptr;
    std::thread consumer([&]{
        result = pool.dqFilledBuf(Ms(2000));
    });
    std::this_thread::sleep_for(Ms(100));
    pool.qFilledBuf(&val);
    consumer.join();
    assert(result == &val);
}

static void test_qFilledBuf_notifies_one_waiter() {
    NVMPI_bufPool<int*> pool;
    int val = 55;
    std::atomic<int> got_item{0};
    std::thread c1([&]{ if(pool.dqFilledBuf(Ms(1000))) got_item.fetch_add(1); });
    std::thread c2([&]{ if(pool.dqFilledBuf(Ms(1000))) got_item.fetch_add(1); });
    std::this_thread::sleep_for(Ms(100));
    pool.qFilledBuf(&val);
    std::this_thread::sleep_for(Ms(200));
    pool.shutdown();
    c1.join();
    c2.join();
    assert(got_item.load() == 1);
}

static void test_concurrent_push_pop_stress() {
    NVMPI_bufPool<int*> pool;
    constexpr int ITEMS_PER_THREAD = 10000;
    constexpr int NUM_PRODUCERS = 4;
    constexpr int NUM_CONSUMERS = 4;
    std::vector<int> items(NUM_PRODUCERS * ITEMS_PER_THREAD, 1);
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> done{false};

    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; p++) {
        producers.emplace_back([&, p]{
            for (int i = 0; i < ITEMS_PER_THREAD; i++) {
                pool.qFilledBuf(&items[p * ITEMS_PER_THREAD + i]);
                produced.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; c++) {
        consumers.emplace_back([&]{
            while (!done.load()) {
                int* item = pool.dqFilledBuf(Ms(100));
                if (item) consumed.fetch_add(1);
            }
            while (int* item = pool.dqFilledBuf()) {
                (void)item;
                consumed.fetch_add(1);
            }
        });
    }

    for (auto& t : producers) t.join();
    std::this_thread::sleep_for(Ms(500));
    done.store(true);
    pool.shutdown();
    for (auto& t : consumers) t.join();

    int total = NUM_PRODUCERS * ITEMS_PER_THREAD;
    assert(produced.load() == total);
    assert(consumed.load() == total); (void)total;
}

int main() {
    std::cout << "=== NVMPI_bufPool unit tests ===" << std::endl;
    RUN_TEST(test_dqFilledBuf_nonblocking_unchanged);
    RUN_TEST(test_dqFilledBuf_blocking_returns_on_push);
    RUN_TEST(test_dqFilledBuf_blocking_returns_null_on_timeout);
    RUN_TEST(test_dqFilledBuf_blocking_returns_null_on_shutdown);
    RUN_TEST(test_shutdown_wakes_all_waiters);
    RUN_TEST(test_reset_clears_shutdown);
    RUN_TEST(test_qFilledBuf_notifies_one_waiter);
    RUN_TEST(test_concurrent_push_pop_stress);
    std::cout << std::endl;
    std::cout << tests_passed << "/" << tests_run << " tests passed." << std::endl;
    return (tests_passed == tests_run) ? 0 : 1;
}
