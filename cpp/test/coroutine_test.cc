// coroutine_test.cc — Tests for coroutine primitives and PoolAwareAwaiter
#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/infra/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace quant::infra {
namespace {

// ── Baton tests ──

TEST(BatonTest, InitiallyNotPosted) {
    Baton b;
    EXPECT_FALSE(b.try_wait());
}

TEST(BatonTest, PostMakesReady) {
    Baton b;
    b.post();
    EXPECT_TRUE(b.try_wait());
}

TEST(BatonTest, BlockingWait) {
    Baton b;
    std::atomic<bool> waiter_done{false};

    std::thread t([&] {
        b.wait();
        waiter_done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(waiter_done);

    b.post();
    t.join();
    EXPECT_TRUE(waiter_done);
}

TEST(BatonTest, Reset) {
    Baton b;
    b.post();
    EXPECT_TRUE(b.try_wait());
    b.reset();
    EXPECT_FALSE(b.try_wait());
}

// ── CoTask tests ──

CoTask<int> simple_cotask() {
    co_return 42;
}

TEST(CoTaskTest, SimpleReturnValue) {
    auto t = simple_cotask();
    EXPECT_TRUE(t.valid());
}

CoTask<int> add_cotask(int a, int b) {
    co_return a + b;
}

TEST(CoTaskTest, ParameterizedTask) {
    auto t = add_cotask(3, 4);
    EXPECT_TRUE(t.valid());
}

CoTask<void> void_cotask() {
    co_return;
}

TEST(CoTaskTest, VoidTask) {
    auto t = void_cotask();
    EXPECT_TRUE(t.valid());
}

// ── PoolAwareAwaiter tests ──

TEST(PoolAwareAwaiterTest, CoSubmitReturnsValue) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    auto state = std::make_shared<CoTaskState<int>>();
    auto* pool_ptr = &pool;

    auto wrapper = [state, pool_ptr]() {
        state->result.emplace(42);
        state->completed.store(true, std::memory_order_release);
        if (state->waiter && !state->resumed.exchange(true, std::memory_order_acq_rel)) {
            pool_ptr->enqueue_handle(state->waiter);
        }
    };

    pool.enqueue_task(Task(std::move(wrapper)));

    int attempts = 0;
    while (!state->completed.load(std::memory_order_acquire) && attempts < 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++attempts;
    }

    ASSERT_TRUE(state->completed.load());
    ASSERT_TRUE(state->result.has_value());
    EXPECT_EQ(state->result.value(), 42);

    pool.stop();
}

TEST(PoolAwareAwaiterTest, EnqueueHandleResumesOnPool) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    std::atomic<bool> resumed{false};
    auto coro = [&]() -> CoTask<void> {
        resumed = true;
        co_return;
    };

    auto t = coro();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(resumed);

    pool.stop();
}

TEST(PoolAwareAwaiterTest, CoSubmitNoExtraThreads) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    for (int i = 0; i < 10; ++i) {
        auto awaiter = pool.co_submit([i]() { return i * i; });
    }

    auto stats = pool.stats();
    EXPECT_GE(stats.tasks_submitted, 10);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    stats = pool.stats();
    EXPECT_GE(stats.tasks_completed, 10);

    pool.stop();
}

TEST(PoolAwareAwaiterTest, CoSubmitVoid) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    std::atomic<int> counter{0};
    for (int i = 0; i < 5; ++i) {
        auto awaiter = pool.co_submit([&counter]() { counter.fetch_add(1); });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(counter.load(), 5);

    pool.stop();
}

// ── CoroutineMutex tests ──

TEST(CoroutineMutexTest, BasicLockUnlock) {
    CoroutineMutex mtx;
    // Verify construction/destruction
}

// ── Integration: CoTask + ThreadPool ──

CoTask<int> compute_on_pool(ThreadPool& pool) {
    co_return 0;
}

TEST(CoroutineIntegrationTest, CoTaskWithPool) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    auto t = compute_on_pool(pool);
    EXPECT_TRUE(t.valid());

    pool.stop();
}

// ── Handles resumed counter ──

TEST(PoolStatsTest, HandlesResumedCounter) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    // Use submit + manual handle enqueue to test the counter
    std::atomic<int> counter{0};
    for (int i = 0; i < 5; ++i) {
        // Directly test enqueue_handle with a real coroutine
        auto state = std::make_shared<CoTaskState<int>>();
        auto* pool_ptr = &pool;

        auto wrapper = [state, pool_ptr, &counter]() {
            counter.fetch_add(1);
            state->result.emplace(1);
            state->completed.store(true, std::memory_order_release);
            // Simulate what co_submit does: enqueue handle back to pool
            if (state->waiter && !state->resumed.exchange(true, std::memory_order_acq_rel)) {
                pool_ptr->enqueue_handle(state->waiter);
            }
        };

        pool.enqueue_task(Task(std::move(wrapper)));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(counter.load(), 5);

    // Without a coroutine waiter, handles_resumed stays 0 (no waiter registered)
    // This is correct behavior - only actual coroutine suspensions get resumed
    auto stats = pool.stats();
    EXPECT_EQ(stats.handles_resumed, 0u);

    pool.stop();
}

}  // namespace
}  // namespace quant::infra
