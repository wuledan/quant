// folly_smoke_test.cc -- Verify Folly integration compiles and runs
// Tests: folly::coro::Task, Baton, Mutex, collectAll
#include <folly/coro/Task.h>
#include <folly/coro/Baton.h>
#include <folly/coro/Mutex.h>
#include <folly/coro/Collect.h>
#include <folly/coro/BlockingWait.h>

#include <gtest/gtest.h>
#include <vector>

using namespace folly::coro;

TEST(FollySmokeTest, TaskReturnsValue) {
    auto task = []() -> Task<int> { co_return 42; };
    EXPECT_EQ(blockingWait(task()), 42);
}

TEST(FollySmokeTest, TaskVoid) {
    bool executed = false;
    auto task = [&]() -> Task<void> {
        executed = true;
        co_return;
    };
    blockingWait(task());
    EXPECT_TRUE(executed);
}

TEST(FollySmokeTest, BatonPostAndWait) {
    Baton baton;
    auto waiter = [&baton]() -> Task<void> {
        co_await baton;
    };
    baton.post();
    blockingWait(waiter());
}

TEST(FollySmokeTest, BatonTryWait) {
    Baton baton;
    EXPECT_FALSE(baton.ready());
    baton.post();
    EXPECT_TRUE(baton.ready());
}

TEST(FollySmokeTest, MutexLockUnlock) {
    Mutex mutex;
    auto task = [&mutex]() -> Task<int> {
        [[maybe_unused]] auto lock = co_await mutex.co_scoped_lock();
        co_return 42;
    };
    EXPECT_EQ(blockingWait(task()), 42);
}

TEST(FollySmokeTest, CollectAll) {
    auto task1 = []() -> Task<int> { co_return 1; };
    auto task2 = []() -> Task<int> { co_return 2; };
    auto [a, b] = blockingWait(collectAll(task1(), task2()));
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
}

TEST(FollySmokeTest, CollectAllRange) {
    std::vector<Task<int>> tasks;
    for (int i = 0; i < 5; ++i) {
        tasks.push_back([](int v) -> Task<int> { co_return v; }(i));
    }
    auto results = blockingWait(collectAllRange(std::move(tasks)));
    ASSERT_EQ(results.size(), 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(results[i], i);
    }
}