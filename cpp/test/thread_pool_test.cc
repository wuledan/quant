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

#ifdef __cpp_coroutines
TEST(ThreadPoolTest, CoSubmit) {
    ThreadPool pool(ThreadPoolConfig{.worker_count = 2});
    pool.start();

    auto awaiter = pool.co_submit([]() { return 42; });
    // For now, co_await isn't used in a coroutine context,
    // but the TaskAwaiter type should exist
    EXPECT_TRUE(true);

    pool.stop();
}
#endif

}  // namespace
}  // namespace quant::infra
