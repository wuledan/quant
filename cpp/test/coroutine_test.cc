// coroutine_test.cc -- Tests for Folly-based coroutine primitives
#include "cpp/quant/infra/coroutine.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace quant::infra {
namespace {

// ── CoTask tests ──

CoTask<int> return_value() { co_return 42; }

TEST(CoTaskTest, ReturnValue) {
    auto result = blockingWait(return_value());
    EXPECT_EQ(result, 42);
}

TEST(CoTaskTest, VoidTask) {
    bool ok = false;
    auto task = [&]() -> CoTask<void> {
        ok = true;
        co_return;
    };
    blockingWait(task());
    EXPECT_TRUE(ok);
}

CoTask<int> add_values(int a, int b) { co_return a + b; }

TEST(CoTaskTest, ParameterizedTask) {
    auto result = blockingWait(add_values(3, 4));
    EXPECT_EQ(result, 7);
}

TEST(CoTaskTest, ChainedTasks) {
    auto task = []() -> CoTask<int> {
        auto a = co_await return_value();
        auto b = co_await add_values(a, 10);
        co_return b;
    };
    auto result = blockingWait(task());
    EXPECT_EQ(result, 52);
}

// ── CoBaton tests (folly::coro::Baton based) ──

TEST(CoBatonTest, InitiallyNotReady) {
    CoBaton b;
    EXPECT_FALSE(b.ready());
}

TEST(CoBatonTest, PostMakesReady) {
    CoBaton b;
    b.post_direct();
    EXPECT_TRUE(b.ready());
}

TEST(CoBatonTest, PostResumesWaiter) {
    CoBaton b;
    std::atomic<bool> resumed{false};
    std::atomic<bool> started{false};

    auto waiter = [&]() -> CoTask<void> {
        started = true;
        co_await b;
        resumed = true;
    };

    // Start the waiter coroutine in a background thread
    std::thread t([&]() { blockingWait(waiter()); });

    // Spin until coroutine starts, then ensure it's suspended on the baton
    while (!started) {
        std::this_thread::yield();
    }
    EXPECT_FALSE(resumed);

    b.post_direct();
    t.join();
    EXPECT_TRUE(resumed);
}

TEST(CoBatonTest, ResetAndReuse) {
    CoBaton b;
    b.post_direct();
    EXPECT_TRUE(b.ready());
    b.reset();
    EXPECT_FALSE(b.ready());
}

// ── CoMutex tests ──

TEST(CoMutexTest, LockUnlock) {
    CoMutex mutex;
    auto task = [&]() -> CoTask<int> {
        auto lock = co_await mutex.co_scoped_lock();
        co_return 42;
    };
    EXPECT_EQ(blockingWait(task()), 42);
}

TEST(CoMutexTest, MutualExclusion) {
    CoMutex mutex;
    int shared = 0;

    auto worker = [&](int delta) -> CoTask<void> {
        [[maybe_unused]] auto lock = co_await mutex.co_scoped_lock();
        shared += delta;
    };

    blockingWait(collectAll(worker(1), worker(2), worker(3)));
    EXPECT_EQ(shared, 6);
}

// ── collectAll tests ──

TEST(CollectAllTest, TwoTasks) {
    auto task1 = []() -> CoTask<int> { co_return 1; };
    auto task2 = []() -> CoTask<int> { co_return 2; };

    auto [a, b] = blockingWait(collectAll(task1(), task2()));
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
}

TEST(CollectAllTest, RangeOfTasks) {
    std::vector<CoTask<int>> tasks;
    for (int i = 0; i < 5; ++i) {
        tasks.push_back([](int v) -> CoTask<int> { co_return v; }(i));
    }

    auto results = blockingWait(collectAllRange(std::move(tasks)));
    ASSERT_EQ(results.size(), 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(results[i], i);
    }
}

// ── Exception handling ──

TEST(CoTaskTest, ExceptionPropagation) {
    auto task = []() -> CoTask<int> {
        throw std::runtime_error("test error");
        co_return 0;
    };
    EXPECT_THROW(blockingWait(task()), std::runtime_error);
}

// ── Combined: CoTask + CoBaton + CollectAll ──

TEST(IntegrationTest, BatonAndCollectAll) {
    CoBaton b1, b2;
    bool done1 = false, done2 = false;

    auto waiter1 = [&]() -> CoTask<void> {
        co_await b1;
        done1 = true;
    };
    auto waiter2 = [&]() -> CoTask<void> {
        co_await b2;
        done2 = true;
    };

    auto all = collectAll(waiter1(), waiter2());
    std::thread t([&]() { blockingWait(std::move(all)); });

    b1.post_direct();
    b2.post_direct();
    t.join();

    EXPECT_TRUE(done1);
    EXPECT_TRUE(done2);
}

}  // namespace
}  // namespace quant::infra
