// integration_test.cc — T10: 调度/线程/利用率三大观测
#include "task_graph.h"
#include "wave_scheduler.h"
#include "work_stealing_executor.h"
#include "coroutine.h"

#include <gtest/gtest.h>
#include <folly/coro/BlockingWait.h>

#include <atomic>
#include <chrono>
#include <future>

using namespace quant::scheduler;
using namespace quant::infra;

// ── 调度观测: WaveScheduler + WorkStealingExecutor 全链路 ──

TEST(IntegrationTest, SchedulingObservation) {
    WorkStealingExecutor ex(4);
    ex.start();

    TaskGraph graph;
    std::atomic<int> counter{0};

    auto a = graph.add_task("A", [&] { counter++; });
    auto b = graph.add_task("B", [&] { counter++; });
    auto c = graph.add_task("C", [&] { counter++; });
    auto d = graph.add_task("D", [&] { counter++; });
    auto e = graph.add_task("E", [&] { counter++; });

    graph.add_dependency(c, a);
    graph.add_dependency(c, b);
    graph.add_dependency(d, a);
    graph.add_dependency(e, c);
    graph.add_dependency(e, d);

    WaveScheduler scheduler;
    auto result = blockingWait(scheduler.execute_async(graph, ex));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.completed_tasks, 5u);
    EXPECT_EQ(counter.load(), 5);

    ex.stop();
}

// ── 线程观测: 确认任务在池线程上运行 ──

TEST(IntegrationTest, ThreadAffinityObservation) {
    WorkStealingExecutor ex(4);
    ex.start();

    std::promise<void> promise;
    auto fut = promise.get_future();
    std::atomic<size_t> task_worker_id{SIZE_MAX};
    std::atomic<bool> is_pool{false};

    ex.add([&]() {
        task_worker_id = WorkStealingExecutor::current_worker_id();
        is_pool = WorkStealingExecutor::is_pool_worker();
        promise.set_value();
    });

    fut.wait_for(std::chrono::seconds(5));

    EXPECT_LT(task_worker_id.load(), ex.worker_count());
    EXPECT_TRUE(is_pool.load());
    EXPECT_FALSE(WorkStealingExecutor::is_pool_worker());
    EXPECT_EQ(WorkStealingExecutor::current_worker_id(),
              WorkStealingExecutor::kExternalThread);

    ex.stop();
}

// ── 利用率观测: 执行负载后检查 stats ──

TEST(IntegrationTest, UtilizationObservation) {
    WorkStealingExecutor ex(4);
    ex.start();

    constexpr int kNumTasks = 500;
    std::atomic<int> remaining{kNumTasks};
    std::promise<void> promise;
    auto fut = promise.get_future();

    for (int i = 0; i < kNumTasks; ++i) {
        ex.add([&]() {
            volatile double x = 1.0;
            for (int j = 0; j < 1000; ++j) x += j * 0.001;
            if (remaining.fetch_sub(1) == 1) {
                promise.set_value();
            }
        });
    }

    fut.wait_for(std::chrono::seconds(10));

    auto stats = ex.stats();
    EXPECT_GT(stats.tasks_completed, 0u);
    EXPECT_GT(stats.active_workers, 0u);
    EXPECT_GT(stats.utilization, 0.0);

    ex.stop();
}

// ── 综合: 多级 DAG + 执行顺序验证 ──

TEST(IntegrationTest, MultiLevelDagScheduling) {
    WorkStealingExecutor ex(4);
    ex.start();

    TaskGraph graph;
    std::atomic<int> exec_order{0};
    int a_ord = 0, b_ord = 0, c_ord = 0, d_ord = 0;
    int e_ord = 0, f_ord = 0, g_ord = 0;

    auto a = graph.add_task("A", [&]() { a_ord = ++exec_order; });
    auto b = graph.add_task("B", [&]() { b_ord = ++exec_order; });
    auto c = graph.add_task("C", [&]() { c_ord = ++exec_order; });
    auto d = graph.add_task("D", [&]() { d_ord = ++exec_order; });
    auto e = graph.add_task("E", [&]() { e_ord = ++exec_order; });
    auto f = graph.add_task("F", [&]() { f_ord = ++exec_order; });
    auto g = graph.add_task("G", [&]() { g_ord = ++exec_order; });

    graph.add_dependency(e, a);
    graph.add_dependency(e, b);
    graph.add_dependency(f, c);
    graph.add_dependency(f, d);
    graph.add_dependency(g, e);
    graph.add_dependency(g, f);

    WaveScheduler scheduler;
    auto result = blockingWait(scheduler.execute_async(graph, ex));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.completed_tasks, 7u);
    EXPECT_EQ(g_ord, 7);

    auto stats = ex.stats();
    EXPECT_GT(stats.utilization, 0.0);

    ex.stop();
}
