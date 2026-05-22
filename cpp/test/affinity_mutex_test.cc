// affinity_mutex_test.cc -- AffinityMutex state and basic tests
//
// Tests cover: try_lock, co_lock, co_scoped_lock (RAII),
// mutual exclusion under contention, and thread affinity routing.
#include "cpp/quant/infra/coroutine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

namespace quant::infra {
namespace {

// ── Basic state tests ──

TEST(AffinityMutexTest, InitiallyUnlocked) {
    AffinityMutex m;
    // Should be able to try_lock successfully
    EXPECT_TRUE(m.try_lock());
    // Must unlock to avoid UB in destructor
    m.unlock();
}

TEST(AffinityMutexTest, TryLockFailsWhenLocked) {
    AffinityMutex m;
    m.try_lock();
    EXPECT_FALSE(m.try_lock());
    m.unlock();
}

TEST(AffinityMutexTest, TryLockUnlockCycle) {
    AffinityMutex m;
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(m.try_lock());
        EXPECT_FALSE(m.try_lock());
        m.unlock();
    }
}

// ── Coroutine lock/unlock ──

TEST(AffinityMutexTest, SingleCoroutineLockUnlock) {
    AffinityMutex m;

    auto task = [&]() -> CoTask<void> {
        co_await m.co_lock();
        // We hold the lock here
        m.unlock();
    };

    blockingWait(task());
}

TEST(AffinityMutexTest, ScopedLockRAII) {
    AffinityMutex m;
    bool was_locked = false;

    auto task = [&]() -> CoTask<void> {
        {
            auto guard = co_await m.co_scoped_lock();
            // Lock is held
            was_locked = true;
            EXPECT_FALSE(m.try_lock());  // Should be locked
        }
        // Lock should be released after guard goes out of scope
        EXPECT_TRUE(m.try_lock());
        m.unlock();
    };

    blockingWait(task());
    EXPECT_TRUE(was_locked);
}

// ── Mutual exclusion under contention ──

TEST(AffinityMutexTest, MutualExclusion) {
    AffinityMutex m;
    std::atomic<int> counter{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> current{0};
    constexpr int kCoroutines = 5;
    constexpr int kIterations = 100;

    auto worker = [&]() -> CoTask<void> {
        for (int i = 0; i < kIterations; ++i) {
            co_await m.co_lock();
            int cur = current.fetch_add(1) + 1;
            // Update max concurrent (should never exceed 1)
            int old_max = max_concurrent.load();
            while (cur > old_max) {
                if (max_concurrent.compare_exchange_weak(old_max, cur)) break;
            }
            counter.fetch_add(1);
            current.fetch_sub(1);
            m.unlock();
        }
    };

    // Run multiple workers concurrently using blockingWait in separate threads
    std::vector<std::thread> threads;
    for (int i = 0; i < kCoroutines; ++i) {
        threads.emplace_back([&]() {
            blockingWait(worker());
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(counter.load(), kCoroutines * kIterations);
    EXPECT_EQ(max_concurrent.load(), 1);  // Mutual exclusion held
}

// ── RAII guard with contention ──

TEST(AffinityMutexTest, ScopedLockContention) {
    AffinityMutex m;
    std::atomic<int> counter{0};
    constexpr int kCoroutines = 4;
    constexpr int kIterations = 50;

    auto worker = [&]() -> CoTask<void> {
        for (int i = 0; i < kIterations; ++i) {
            auto guard = co_await m.co_scoped_lock();
            counter.fetch_add(1);
            // Guard releases lock at scope exit
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kCoroutines; ++i) {
        threads.emplace_back([&]() {
            blockingWait(worker());
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(counter.load(), kCoroutines * kIterations);
}

// ── Unlock with no waiters ──

TEST(AffinityMutexTest, UnlockNoWaiters) {
    AffinityMutex m;
    m.try_lock();
    m.unlock();
    // Should be able to lock again
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

// ── Destroy without unlock (should not crash) ──

TEST(AffinityMutexTest, DestroyWhileLocked) {
    {
        AffinityMutex m;
        m.try_lock();
        // Destroy while locked — should not crash
    }
}

// ── Lock acquire after unlock when waiters present ──

TEST(AffinityMutexTest, WaiterGetsLockAfterUnlock) {
    AffinityMutex m;
    std::atomic<bool> first_holds_lock{false};
    std::atomic<bool> second_got_lock{false};
    AffinityBaton first_done;
    AffinityBaton second_started;

    auto first = [&]() -> CoTask<void> {
        co_await m.co_lock();
        first_holds_lock = true;
        // Wait for second to start waiting
        co_await second_started;
        // Hold the lock for a moment
        m.unlock();
        first_done.post_direct();
    };

    auto second = [&]() -> CoTask<void> {
        second_started.post_direct();
        co_await m.co_lock();
        second_got_lock = true;
        m.unlock();
    };

    // Run in separate threads
    std::thread t1([&]() { blockingWait(first()); });
    std::thread t2([&]() { blockingWait(second()); });

    t1.join();
    t2.join();

    EXPECT_TRUE(first_holds_lock);
    EXPECT_TRUE(second_got_lock);
}

// ── Guard move semantics ──

TEST(AffinityMutexTest, GuardMoveSemantics) {
    AffinityMutex m;

    auto task = [&]() -> CoTask<void> {
        auto guard1 = co_await m.co_scoped_lock();
        // Lock is held, guard1 owns it

        // Move guard1 to guard2
        auto guard2 = std::move(guard1);
        // guard1 is now null, guard2 owns the lock

        // guard2 releases lock at scope exit
    };

    blockingWait(task());
    // After task completes, lock should be available
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

}  // namespace
}  // namespace quant::infra
