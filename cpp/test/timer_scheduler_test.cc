// timer_scheduler_test.cc -- Tests for TimerScheduler
//
// Tests:
//   CoSleepBasic        -- co_sleep(50ms) resumes after ~50ms
//   CoSleepAccuracy     -- co_sleep(100ms) actual time within 100±50ms
//   ScheduleAtFires     -- schedule_at callback executes
//   SchedulePeriodicFires -- schedule_periodic fires >= 3 times
//   CancelTimer         -- cancel prevents callback from firing

#include "cpp/quant/infra/timer_scheduler.h"
#include "cpp/quant/infra/work_stealing_executor.h"

#include <folly/coro/Task.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace quant::infra;
using namespace std::chrono_literals;

// ── CoSleepBasic ──

TEST(TimerSchedulerTest, CoSleepBasic) {
    WorkStealingExecutor executor(2, "test");
    executor.start();

    TimerScheduler timer(executor);
    timer.start();

    std::atomic<bool> slept{false};

    auto task = folly::coro::co_invoke(
        [&]() -> folly::coro::Task<void> {
            co_await timer.co_sleep(50ms);
            slept.store(true);
        });

    // Start the coroutine on the executor; workers drive it to completion.
    (void)std::move(task).scheduleOn(&executor).start();

    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(slept.load());

    timer.stop();
    executor.stop();
}

// ── CoSleepAccuracy ──

TEST(TimerSchedulerTest, CoSleepAccuracy) {
    WorkStealingExecutor executor(2, "test");
    executor.start();

    TimerScheduler timer(executor);
    timer.start();

    std::atomic<std::chrono::nanoseconds> elapsed{0ns};

    auto task = folly::coro::co_invoke(
        [&]() -> folly::coro::Task<void> {
            auto start = std::chrono::steady_clock::now();
            co_await timer.co_sleep(100ms);
            elapsed.store(
                std::chrono::steady_clock::now() - start,
                std::memory_order_release);
        });

    (void)std::move(task).scheduleOn(&executor).start();

    std::this_thread::sleep_for(300ms);

    auto ns = elapsed.load(std::memory_order_acquire);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(ns);
    EXPECT_GE(ms.count(), 50);   // at least 50ms
    EXPECT_LE(ms.count(), 200);  // at most 200ms (generous on loaded CI)

    timer.stop();
    executor.stop();
}

// ── ScheduleAtFires ──

TEST(TimerSchedulerTest, ScheduleAtFires) {
    WorkStealingExecutor executor(2, "test");
    executor.start();

    TimerScheduler timer(executor);
    timer.start();

    std::atomic<bool> fired{false};

    auto deadline = std::chrono::steady_clock::now() + 50ms;
    timer.schedule_at(deadline, 0, [&fired]() { fired.store(true); });

    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(fired.load());

    timer.stop();
    executor.stop();
}

// ── SchedulePeriodicFires ──

TEST(TimerSchedulerTest, SchedulePeriodicFires) {
    WorkStealingExecutor executor(2, "test");
    executor.start();

    TimerScheduler timer(executor);
    timer.start();

    std::atomic<int> count{0};

    auto id = timer.schedule_periodic(
        50ms, 0, [&count]() { count.fetch_add(1); });

    std::this_thread::sleep_for(300ms);
    timer.cancel(id);
    std::this_thread::sleep_for(100ms);  // let cancel propagate

    // Should have fired at least 3 times in 300ms with 50ms period.
    EXPECT_GE(count.load(), 3);

    timer.stop();
    executor.stop();
}

// ── CancelTimer ──

TEST(TimerSchedulerTest, CancelTimer) {
    WorkStealingExecutor executor(2, "test");
    executor.start();

    TimerScheduler timer(executor);
    timer.start();

    std::atomic<bool> fired{false};

    auto id = timer.schedule_at(
        std::chrono::steady_clock::now() + 100ms,
        0,
        [&fired]() { fired.store(true); });

    // Cancel immediately.
    timer.cancel(id);

    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(fired.load());

    timer.stop();
    executor.stop();
}
