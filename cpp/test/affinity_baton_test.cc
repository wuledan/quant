// affinity_baton_test.cc -- AffinityBaton state and basic tests
//
// Full executor-routing tests depend on WorkStealingExecutor (T5).
#include "cpp/quant/infra/coroutine.h"

#include <gtest/gtest.h>

namespace quant::infra {
namespace {

TEST(AffinityBatonTest, InitiallyNotReady) {
    AffinityBaton b;
    EXPECT_FALSE(b.ready());
    EXPECT_FALSE(b.try_wait());
}

TEST(AffinityBatonTest, PostMakesReady) {
    AffinityBaton b;
    b.post_direct();
    EXPECT_TRUE(b.ready());
    EXPECT_TRUE(b.try_wait());
}

TEST(AffinityBatonTest, ResetAndReuse) {
    AffinityBaton b;
    b.post_direct();
    EXPECT_TRUE(b.ready());
    b.reset();
    EXPECT_FALSE(b.ready());
    b.post_direct();
    EXPECT_TRUE(b.ready());
}

TEST(AffinityBatonTest, PostDirectNoCrashWhenNoWaiters) {
    AffinityBaton b;
    b.post_direct();
    EXPECT_TRUE(b.ready());
}

TEST(AffinityBatonTest, DestroyWithoutPostDoesNotCrash) {
    AffinityBaton b;
    // Destroying without posting should be safe
}

TEST(AffinityBatonTest, AwaiterReadyDoesNotSuspend) {
    AffinityBaton b;
    b.post_direct();

    auto task = [&]() -> CoTask<void> {
        // Should not suspend since baton is already ready
        co_await b;
    };

    blockingWait(task());
    EXPECT_TRUE(b.ready());
}

TEST(AffinityBatonTest, CoAwaitAndPostDirect) {
    AffinityBaton b;
    std::atomic<bool> resumed{false};

    auto waiter = [&]() -> CoTask<void> {
        co_await b;
        resumed = true;
    };

    // Start waiter in background, then post_direct
    std::thread t([&]() { blockingWait(waiter()); });
    b.post_direct();
    t.join();
    EXPECT_TRUE(resumed);
}

// ── [needs-executor] Executor-routed post tests ──
// Will be added when WorkStealingExecutor (T5) is implemented.

}  // namespace
}  // namespace quant::infra
