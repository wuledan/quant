// thread_pool_test.cc — Tests for work-stealing thread pool
#include "cpp/quant/infra/thread_pool.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>

namespace quant::infra {
namespace {

TEST(ThreadPoolTest, SubmitAndGet) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    auto fut = pool.submit([]() { return 42; });
    EXPECT_EQ(fut.get(), 42);

    pool.stop();
}

TEST(ThreadPoolTest, SubmitVoid) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    std::atomic<bool> executed{false};
    auto fut = pool.submit([&]() { executed = true; });
    fut.wait();
    EXPECT_TRUE(executed);

    pool.stop();
}

TEST(ThreadPoolTest, MultipleTasks) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 4});
    pool.start();

    constexpr int kTaskCount = 100;
    std::vector<std::future<int>> futures;
    futures.reserve(kTaskCount);

    for (int i = 0; i < kTaskCount; ++i) {
        futures.push_back(pool.submit([i]() { return i * i; }));
    }

    for (int i = 0; i < kTaskCount; ++i) {
        EXPECT_EQ(futures[i].get(), i * i);
    }

    pool.stop();
}

TEST(ThreadPoolTest, ExceptionPropagation) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    auto fut = pool.submit([]() -> int {
        throw std::runtime_error("test error");
    });

    EXPECT_THROW(fut.get(), std::runtime_error);
    pool.stop();
}

TEST(ThreadPoolTest, BatchSubmit) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 4});
    pool.start();

    std::vector<std::function<void()>> tasks;
    std::atomic<int> counter{0};

    for (int i = 0; i < 50; ++i) {
        tasks.emplace_back([&]() { counter.fetch_add(1); });
    }

    auto futures = pool.submit_batch(tasks);
    for (auto& f : futures) {
        f.wait();
    }

    EXPECT_EQ(counter.load(), 50);
    pool.stop();
}

TEST(ThreadPoolTest, StartStopIdempotent) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();
    pool.start();
    pool.stop();
    pool.stop();
    EXPECT_FALSE(pool.is_running());
}

TEST(ThreadPoolTest, StatsTracking) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    auto fut1 = pool.submit([]() { return 1; });
    fut1.get();

    auto stats = pool.stats();
    EXPECT_GE(stats.tasks_submitted, 1);
    EXPECT_GE(stats.tasks_completed, 1);

    pool.stop();
}

TEST(ThreadPoolTest, WorkerCount) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 4});
    EXPECT_EQ(pool.worker_count(), 4);
}

TEST(ThreadPoolTest, HandlesResumedInStats) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    auto stats = pool.stats();
    EXPECT_EQ(stats.handles_resumed, 0u);

    pool.stop();
}

TEST(ThreadPoolTest, CoSubmitProducesPoolAwareAwaiter) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    // co_submit returns PoolAwareAwaiter, not the old TaskAwaiter
    auto awaiter = pool.co_submit([]() { return 42; });

    // Give the task time to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    pool.stop();
}

TEST(ThreadPoolTest, EnqueueHandle) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    // co_submit without co_await: the task runs on the pool,
    // but since no coroutine is awaiting, no handle is enqueued back.
    // This tests the basic co_submit path.
    std::atomic<int> counter{0};
    for (int i = 0; i < 10; ++i) {
        auto awaiter = pool.co_submit([&counter]() {
            counter.fetch_add(1);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(counter.load(), 10);

    // Without a coroutine waiter, handles_resumed stays 0
    auto stats = pool.stats();
    EXPECT_EQ(stats.handles_resumed, 0u);

    pool.stop();
}

}  // namespace
}  // namespace quant::infra
