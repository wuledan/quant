// wave_scheduler_co_test.cc — Tests for WaveScheduler async execution
#include "wave_scheduler.h"
#include "task_graph.h"
#include "work_stealing_executor.h"
#include "coroutine.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <folly/coro/BlockingWait.h>

using namespace quant::scheduler;
using namespace quant::infra;

static WorkStealingExecutor make_executor(size_t workers = 4) {
    return WorkStealingExecutor(workers, "ws_co_test");
}

TEST(WaveSchedulerCoTest, ExecuteAsyncSimpleTasks) {
    auto executor = make_executor();
    executor.start();

    TaskGraph graph;
    std::atomic<int> counter{0};

    graph.add_task("T1", [&] { counter++; });
    graph.add_task("T2", [&] { counter++; });

    WaveScheduler scheduler;
    auto result = blockingWait(scheduler.execute_async(graph, executor));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_tasks, 2u);
    EXPECT_EQ(result.completed_tasks, 2u);
    EXPECT_EQ(counter.load(), 2);

    executor.stop();
}

TEST(WaveSchedulerCoTest, ExecuteAsyncWithDependencies) {
    auto executor = make_executor();
    executor.start();

    TaskGraph graph;
    std::atomic<int> order{0};
    int a_order = 0, b_order = 0, c_order = 0;

    auto a = graph.add_task("A", [&] { a_order = ++order; });
    auto b = graph.add_task("B", [&] { b_order = ++order; });
    auto c = graph.add_task("C", [&] { c_order = ++order; });

    graph.add_dependency(c, b);
    graph.add_dependency(b, a);

    WaveScheduler scheduler;
    auto result = blockingWait(scheduler.execute_async(graph, executor));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.completed_tasks, 3u);
    EXPECT_EQ(a_order, 1);
    EXPECT_EQ(b_order, 2);
    EXPECT_EQ(c_order, 3);

    executor.stop();
}

TEST(WaveSchedulerCoTest, ExecuteAsyncTaskSubset) {
    auto executor = make_executor();
    executor.start();

    TaskGraph graph;
    std::atomic<int> counter{0};

    auto a = graph.add_task("A", [&] { counter++; });
    auto b = graph.add_task("B", [&] { counter++; });
    graph.add_task("C", [&] { counter++; });

    graph.add_dependency(b, a);

    WaveScheduler scheduler;
    auto result = blockingWait(scheduler.execute_tasks_async(graph, {b}, executor));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(counter.load(), 2);

    executor.stop();
}

TEST(WaveSchedulerCoTest, ExecuteAsyncInvalidGraph) {
    auto executor = make_executor();
    executor.start();

    TaskGraph graph;
    auto a = graph.add_task("A", []{});
    auto b = graph.add_task("B", []{});
    auto c = graph.add_task("C", []{});

    graph.add_dependency(a, b);
    graph.add_dependency(b, c);
    graph.add_dependency(c, a);

    WaveScheduler scheduler;
    auto result = blockingWait(scheduler.execute_async(graph, executor));

    EXPECT_FALSE(result.success);

    executor.stop();
}

TEST(WaveSchedulerCoTest, SyncExecutionStillWorks) {
    TaskGraph graph;
    std::atomic<int> counter{0};

    graph.add_task("T1", [&] { counter++; });
    graph.add_task("T2", [&] { counter++; });

    WaveScheduler scheduler;
    auto result = scheduler.execute(graph);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.completed_tasks, 2u);
    EXPECT_EQ(counter.load(), 2);
}
