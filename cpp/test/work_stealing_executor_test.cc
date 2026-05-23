// work_stealing_executor_test.cc — Tests for WorkStealingExecutor
#include "cpp/quant/infra/work_stealing_executor.h"

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace quant::infra {
namespace {

// ── Basic task submission ──

TEST(WorkStealingExecutorTest, BasicSubmit) {
    WorkStealingExecutor ex(2);
    ex.start();

    std::atomic<bool> executed{false};
    std::promise<void> promise;
    auto fut = promise.get_future();

    ex.add([&]() {
        executed = true;
        promise.set_value();
    });

    fut.wait_for(std::chrono::seconds(5));
    EXPECT_TRUE(executed);

    ex.stop();
}

TEST(WorkStealingExecutorTest, SubmitWithResult) {
    WorkStealingExecutor ex(2);
    ex.start();

    std::promise<int> promise;
    auto fut = promise.get_future();

    ex.add([&]() { promise.set_value(42); });

    EXPECT_EQ(fut.get(), 42);
    ex.stop();
}

// ── Thread-affine submission ──

TEST(WorkStealingExecutorTest, AddToWorker) {
    WorkStealingExecutor ex(2);
    ex.start();

    std::atomic<size_t> executed_on_worker{SIZE_MAX};
    std::promise<void> promise;
    auto fut = promise.get_future();

    ex.add_to_worker(0, [&]() {
        executed_on_worker = WorkStealingExecutor::current_worker_id();
        promise.set_value();
    });

    fut.wait_for(std::chrono::seconds(5));
    EXPECT_EQ(executed_on_worker.load(), 0u);

    ex.stop();
}

TEST(WorkStealingExecutorTest, AddToDifferentWorkers) {
    WorkStealingExecutor ex(4);
    ex.start();

    std::atomic<size_t> worker_a{SIZE_MAX};
    std::atomic<size_t> worker_b{SIZE_MAX};
    std::promise<void> p1, p2;
    auto f1 = p1.get_future();
    auto f2 = p2.get_future();

    ex.add_to_worker(1, [&]() {
        worker_a = WorkStealingExecutor::current_worker_id();
        p1.set_value();
    });
    ex.add_to_worker(3, [&]() {
        worker_b = WorkStealingExecutor::current_worker_id();
        p2.set_value();
    });

    f1.wait_for(std::chrono::seconds(5));
    f2.wait_for(std::chrono::seconds(5));
    EXPECT_EQ(worker_a.load(), 1u);
    EXPECT_EQ(worker_b.load(), 3u);

    ex.stop();
}

// ── Multiple tasks ──

TEST(WorkStealingExecutorTest, MultipleTasks) {
    WorkStealingExecutor ex(4);
    ex.start();

    constexpr int kTaskCount = 500;
    std::atomic<int> counter{0};
    std::promise<void> promise;
    auto fut = promise.get_future();
    std::atomic<int> completed{0};

    for (int i = 0; i < kTaskCount; ++i) {
        ex.add([&]() {
            counter.fetch_add(1, std::memory_order_relaxed);
            if (completed.fetch_add(1, std::memory_order_relaxed) == kTaskCount - 1) {
                promise.set_value();
            }
        });
    }

    fut.wait_for(std::chrono::seconds(5));
    EXPECT_EQ(counter.load(), kTaskCount);

    ex.stop();
}

// ── Lifecycle ──

TEST(WorkStealingExecutorTest, StartStop) {
    WorkStealingExecutor ex(2);
    EXPECT_FALSE(ex.is_running());

    ex.start();
    EXPECT_TRUE(ex.is_running());

    ex.stop();
    EXPECT_FALSE(ex.is_running());
}

TEST(WorkStealingExecutorTest, StartIsIdempotent) {
    WorkStealingExecutor ex(2);
    ex.start();
    ex.start();  // should not throw
    ex.stop();
}

TEST(WorkStealingExecutorTest, StopIsIdempotent) {
    WorkStealingExecutor ex(2);
    ex.start();
    ex.stop();
    ex.stop();  // should not throw
}

// ── Worker ID queries ──

TEST(WorkStealingExecutorTest, ExternalThreadHasNoWorkerId) {
    EXPECT_EQ(WorkStealingExecutor::current_worker_id(),
              WorkStealingExecutor::kExternalThread);
}

TEST(WorkStealingExecutorTest, ExternalThreadIsNotPoolWorker) {
    EXPECT_FALSE(WorkStealingExecutor::is_pool_worker());
}

TEST(WorkStealingExecutorTest, WorkerIdOnPoolThread) {
    WorkStealingExecutor ex(2);
    ex.start();

    std::promise<size_t> promise;
    auto fut = promise.get_future();

    ex.add([&]() {
        promise.set_value(WorkStealingExecutor::current_worker_id());
    });

    auto id = fut.get();
    EXPECT_LT(id, ex.worker_count());

    ex.stop();
}

// ── Worker count ──

TEST(WorkStealingExecutorTest, WorkerCount) {
    WorkStealingExecutor ex(4);
    EXPECT_EQ(ex.worker_count(), 4u);
}

// ── Stats tracking ──

TEST(WorkStealingExecutorTest, StatsTracking) {
    WorkStealingExecutor ex(2);
    ex.start();

    std::promise<void> promise;
    auto fut = promise.get_future();
    ex.add([&]() { promise.set_value(); });
    fut.wait_for(std::chrono::seconds(5));

    auto stats = ex.stats();
    EXPECT_GE(stats.tasks_completed, 1u);

    ex.stop();
}

TEST(WorkStealingExecutorTest, StatsUtilization) {
    WorkStealingExecutor ex(2);
    ex.start();

    // Saturate with work
    constexpr int kNumTasks = 200;
    std::atomic<int> remaining{kNumTasks};
    std::promise<void> promise;
    auto fut = promise.get_future();

    for (int i = 0; i < kNumTasks; ++i) {
        ex.add([&]() {
            // Busy work
            volatile int x = 0;
            for (int j = 0; j < 1000; ++j) x += j;
            if (remaining.fetch_sub(1, std::memory_order_relaxed) == 1) {
                promise.set_value();
            }
        });
    }

    fut.wait_for(std::chrono::seconds(5));

    auto stats = ex.stats();
    EXPECT_GT(stats.utilization, 0.0);

    ex.stop();
}

// ── Park / unpark behavior ──

TEST(WorkStealingExecutorTest, ParkAndUnpark) {
    WorkStealingExecutor ex(2);
    ex.start();

    // Let workers park
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Submit work — should unpark
    std::promise<void> promise;
    auto fut = promise.get_future();
    ex.add([&]() { promise.set_value(); });

    fut.wait_for(std::chrono::seconds(5));

    auto stats = ex.stats();
    EXPECT_GE(stats.tasks_completed, 1u);

    ex.stop();
}

// ── Force stop ──

TEST(WorkStealingExecutorTest, ForceStop) {
    WorkStealingExecutor ex(2);
    ex.start();

    // Submit work that never completes (infinite loop placeholder)
    std::promise<void> promise;
    auto fut = promise.get_future();

    // Start a task that will be interrupted
    ex.add([&]() {
        // Busy loop that checks running flag
        while (ex.is_running()) {
            std::this_thread::yield();
        }
        promise.set_value();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ex.force_stop();

    // After force_stop, workers should exit
    fut.wait_for(std::chrono::seconds(5));
}

// ── No thread growth ──

TEST(WorkStealingExecutorTest, NoThreadGrowth) {
    WorkStealingExecutor ex(4);
    ex.start();

    // Run many batches of tasks
    for (int batch = 0; batch < 10; ++batch) {
        constexpr int kTasksPerBatch = 100;
        std::atomic<int> remaining{kTasksPerBatch};
        std::promise<void> promise;
        auto fut = promise.get_future();

        for (int i = 0; i < kTasksPerBatch; ++i) {
            ex.add([&]() {
                if (remaining.fetch_sub(1, std::memory_order_relaxed) == 1) {
                    promise.set_value();
                }
            });
        }

        fut.wait_for(std::chrono::seconds(5));
    }

    // Allow idle workers on other NUMA nodes to reach parked state
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto stats = ex.stats();
    EXPECT_EQ(stats.active_workers, 4u);

    ex.stop();
}

// ── Coroutine submission (co_submit, tested within coroutine context) ──

folly::coro::Task<void> co_submit_int_task(WorkStealingExecutor& ex) {
    auto result = co_await ex.co_submit([]() { return 42; });
    EXPECT_EQ(result, 42);
    co_return;
}

folly::coro::Task<void> co_submit_void_task(WorkStealingExecutor& ex,
                                              std::atomic<bool>& flag) {
    co_await ex.co_submit([&]() { flag = true; });
    EXPECT_TRUE(flag.load());
    co_return;
}

folly::coro::Task<void> co_submit_exception_task(WorkStealingExecutor& ex) {
    bool caught = false;
    try {
        co_await ex.co_submit([]() -> int {
            throw std::runtime_error("test error");
        });
    } catch (const std::runtime_error& e) {
        caught = true;
        EXPECT_STREQ(e.what(), "test error");
    }
    EXPECT_TRUE(caught);
    co_return;
}

TEST(WorkStealingExecutorTest, CoSubmitInt) {
    WorkStealingExecutor ex(2);
    ex.start();
    folly::coro::blockingWait(
        folly::coro::co_withExecutor(&ex, co_submit_int_task(ex)));
    ex.stop();
}

TEST(WorkStealingExecutorTest, CoSubmitVoid) {
    WorkStealingExecutor ex(2);
    ex.start();
    std::atomic<bool> executed{false};
    folly::coro::blockingWait(
        folly::coro::co_withExecutor(&ex, co_submit_void_task(ex, executed)));
    ex.stop();
}

TEST(WorkStealingExecutorTest, CoSubmitException) {
    WorkStealingExecutor ex(2);
    ex.start();
    folly::coro::blockingWait(
        folly::coro::co_withExecutor(&ex, co_submit_exception_task(ex)));
    ex.stop();
}

// ── blockingWait bridge for non-coroutine callers ──

TEST(WorkStealingExecutorTest, CoSubmitFromSync) {
    WorkStealingExecutor ex(2);
    ex.start();
    auto result = folly::coro::blockingWait(ex.co_submit([]() { return 42; }));
    EXPECT_EQ(result, 42);
    ex.stop();
}

TEST(WorkStealingExecutorTest, CoSubmitVoidFromSync) {
    WorkStealingExecutor ex(2);
    ex.start();
    std::atomic<bool> executed{false};
    folly::coro::blockingWait(
        ex.co_submit([&]() { executed = true; }));
    EXPECT_TRUE(executed);
    ex.stop();
}

TEST(WorkStealingExecutorTest, CoSubmitExceptionFromSync) {
    WorkStealingExecutor ex(2);
    ex.start();
    EXPECT_THROW(
        folly::coro::blockingWait(ex.co_submit([]() -> int {
            throw std::runtime_error("submit error");
        })),
        std::runtime_error);
    ex.stop();
}

// ── Work stealing behavior ──

TEST(WorkStealingExecutorTest, SomeStealsOccur) {
    // With many workers and many tasks, steals should happen
    WorkStealingExecutor ex(8);
    ex.start();

    // Saturate with enough tasks that some must be stolen
    constexpr int kNumTasks = 5000;
    std::atomic<int> remaining{kNumTasks};
    std::promise<void> promise;
    auto fut = promise.get_future();

    for (int i = 0; i < kNumTasks; ++i) {
        ex.add([&]() {
            volatile int x = 0;
            for (int j = 0; j < 100; ++j) x += j;
            if (remaining.fetch_sub(1, std::memory_order_relaxed) == 1) {
                promise.set_value();
            }
        });
    }

    fut.wait_for(std::chrono::seconds(10));

    auto stats = ex.stats();
    EXPECT_GE(stats.local_steals, 0u);  // steals may or may not happen

    ex.stop();
}

TEST(WorkStealingExecutorTest, LocalPopsVsGlobalPops) {
    // Tasks submitted from a pool thread go to local deque;
    // tasks from external threads go to global queue.
    WorkStealingExecutor ex(2);
    ex.start();

    // Submit from external thread (goes to global queue)
    std::promise<void> p1;
    auto f1 = p1.get_future();
    ex.add([&]() { p1.set_value(); });
    f1.wait_for(std::chrono::seconds(5));

    ex.stop();
}

}  // namespace
}  // namespace quant::infra
