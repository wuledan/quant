// scheduler_test.cc — Tests for TaskGraph, WaveScheduler, CronScheduler, SchedulerService
#include "cpp/quant/scheduler/task_graph.h"
#include "cpp/quant/scheduler/wave_scheduler.h"
#include "cpp/quant/scheduler/cron_scheduler.h"
#include "cpp/quant/scheduler/scheduler_service.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>

namespace quant::scheduler {
namespace {

// ========================================================================
// TaskGraph tests
// ========================================================================

TEST(TaskGraphTest, AddTask) {
    TaskGraph graph;
    auto id = graph.add_task("test", []{});
    EXPECT_GT(id, 0u);
    EXPECT_EQ(graph.size(), 1u);

    auto* task = graph.get_task(id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->name, "test");
    EXPECT_EQ(task->status.load(), TaskStatus::kPending);
}

TEST(TaskGraphTest, AddDependency) {
    TaskGraph graph;
    auto a = graph.add_task("A", []{});
    auto b = graph.add_task("B", []{});

    EXPECT_TRUE(graph.add_dependency(b, a));
    EXPECT_FALSE(graph.add_dependency(b, a));  // duplicate
    EXPECT_FALSE(graph.add_dependency(a, 999));  // non-existent
    EXPECT_FALSE(graph.add_dependency(a, a));    // self-dependency

    auto b_deps = graph.get_dependencies(b);
    ASSERT_EQ(b_deps.size(), 1u);
    EXPECT_EQ(b_deps[0], a);

    auto a_deps = graph.get_dependents(a);
    ASSERT_EQ(a_deps.size(), 1u);
    EXPECT_EQ(a_deps[0], b);
}

TEST(TaskGraphTest, TopologicalSort) {
    TaskGraph graph;
    auto a = graph.add_task("A", []{});
    auto b = graph.add_task("B", []{});
    auto c = graph.add_task("C", []{});

    graph.add_dependency(c, b);
    graph.add_dependency(b, a);

    auto order = graph.topological_sort();
    ASSERT_EQ(order.size(), 3u);
    // A must come before B, B before C
    auto pos_a = std::find(order.begin(), order.end(), a);
    auto pos_b = std::find(order.begin(), order.end(), b);
    auto pos_c = std::find(order.begin(), order.end(), c);
    EXPECT_LT(pos_a, pos_b);
    EXPECT_LT(pos_b, pos_c);
}

TEST(TaskGraphTest, CycleDetection) {
    TaskGraph graph;
    auto a = graph.add_task("A", []{});
    auto b = graph.add_task("B", []{});
    auto c = graph.add_task("C", []{});

    graph.add_dependency(a, b);
    graph.add_dependency(b, c);
    graph.add_dependency(c, a);

    auto result = graph.validate();
    EXPECT_FALSE(result.valid);
}

TEST(TaskGraphTest, ParallelLevels) {
    TaskGraph graph;
    auto a = graph.add_task("A", []{});
    auto b = graph.add_task("B", []{});
    auto c = graph.add_task("C", []{});

    graph.add_dependency(c, a);
    graph.add_dependency(c, b);

    auto levels = graph.parallel_levels();
    EXPECT_GE(levels.size(), 2u);
    // Level 0 should have A and B
    EXPECT_EQ(levels[0].size(), 2u);
}

TEST(TaskGraphTest, Clear) {
    TaskGraph graph;
    graph.add_task("A", []{});
    graph.add_task("B", []{});
    EXPECT_EQ(graph.size(), 2u);

    graph.clear();
    EXPECT_EQ(graph.size(), 0u);
}

// ========================================================================
// WaveScheduler tests
// ========================================================================

TEST(WaveSchedulerTest, ExecuteSimpleTasks) {
    TaskGraph graph;
    std::atomic<int> counter{0};

    graph.add_task("T1", [&]{ counter++; });
    graph.add_task("T2", [&]{ counter++; });

    WaveScheduler scheduler;
    auto result = scheduler.execute(graph);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_tasks, 2u);
    EXPECT_EQ(result.completed_tasks, 2u);
    EXPECT_EQ(result.failed_tasks, 0u);
    EXPECT_EQ(counter.load(), 2);
}

TEST(WaveSchedulerTest, ExecuteWithDependencies) {
    TaskGraph graph;
    std::atomic<int> order{0};
    int a_order = 0, b_order = 0, c_order = 0;

    auto a = graph.add_task("A", [&]{ a_order = ++order; });
    auto b = graph.add_task("B", [&]{ b_order = ++order; });
    auto c = graph.add_task("C", [&]{ c_order = ++order; });

    graph.add_dependency(c, b);
    graph.add_dependency(b, a);

    WaveScheduler scheduler;
    auto result = scheduler.execute(graph);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.completed_tasks, 3u);
    EXPECT_EQ(a_order, 1);
    EXPECT_EQ(b_order, 2);
    EXPECT_EQ(c_order, 3);
}

TEST(WaveSchedulerTest, InvalidGraph) {
    TaskGraph graph;
    auto a = graph.add_task("A", []{});
    auto b = graph.add_task("B", []{});
    auto c = graph.add_task("C", []{});

    graph.add_dependency(a, b);
    graph.add_dependency(b, c);
    graph.add_dependency(c, a);

    WaveScheduler scheduler;
    auto result = scheduler.execute(graph);

    EXPECT_FALSE(result.success);
}

TEST(WaveSchedulerTest, TaskFailure) {
    TaskGraph graph;

    graph.add_task("GOOD", []{});
    graph.add_task("BAD", []{
        throw std::runtime_error("task failed");
    });

    WaveScheduler scheduler;
    auto result = scheduler.execute(graph);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.completed_tasks, 1u);
    EXPECT_EQ(result.failed_tasks, 1u);
}

TEST(WaveSchedulerTest, ExecuteTaskSubset) {
    TaskGraph graph;
    std::atomic<int> counter{0};

    auto a = graph.add_task("A", [&]{ counter++; });
    auto b = graph.add_task("B", [&]{ counter++; });
    graph.add_task("C", [&]{ counter++; });  // excluded

    graph.add_dependency(b, a);

    WaveScheduler scheduler;
    auto result = scheduler.execute_tasks(graph, {b});

    EXPECT_TRUE(result.success);
    EXPECT_EQ(counter.load(), 2);  // A + B
}

// ========================================================================
// CronScheduler tests
// ========================================================================

TEST(CronSchedulerTest, EveryMinuteMatch) {
    // "* * * * *" matches every minute
    int64_t ts = 1700000000;  // roughly 2023-11-14
    EXPECT_TRUE(CronScheduler::matches("* * * * *", ts));
}

TEST(CronSchedulerTest, SpecificMinuteMatch) {
    // Match minute 30
    // 1700000000 = 2023-11-14 22:13:20 UTC
    EXPECT_FALSE(CronScheduler::matches("30 * * * *", 1700000000));

    // Find a timestamp with minute = 30
    int64_t next = CronScheduler::next_match("30 * * * *", 1700000000);
    EXPECT_GT(next, 1700000000);
    EXPECT_TRUE(CronScheduler::matches("30 * * * *", next));
}

TEST(CronSchedulerTest, NextMatchSameHour) {
    // Next minute 45 within the same hour
    int64_t base = 1700000000;
    int64_t next = CronScheduler::next_match("45 * * * *", base);
    EXPECT_GT(next, base);
    EXPECT_TRUE(CronScheduler::matches("45 * * * *", next));
}

TEST(CronSchedulerTest, NextMatchRange) {
    int64_t base = 1700000000;
    int64_t next = CronScheduler::next_match("5,10,15 * * * *", base);
    EXPECT_GT(next, base);
    EXPECT_TRUE(CronScheduler::matches("5,10,15 * * * *", next));
}

TEST(CronSchedulerTest, AddRemoveJobs) {
    CronScheduler cron;
    std::atomic<int> count{0};

    auto id = cron.add_job("test", "* * * * *", [&]{ count++; });
    EXPECT_GT(id, 0u);

    auto jobs = cron.list_jobs();
    EXPECT_EQ(jobs.size(), 1u);
    EXPECT_EQ(jobs[0].name, "test");

    EXPECT_TRUE(cron.remove_job(id));
    EXPECT_FALSE(cron.remove_job(id));
    EXPECT_EQ(cron.list_jobs().size(), 0u);
}

TEST(CronSchedulerTest, EnableDisable) {
    CronScheduler cron;
    auto id = cron.add_job("test", "* * * * *", []{});

    cron.enable_job(id, false);
    auto jobs = cron.list_jobs();
    EXPECT_FALSE(jobs[0].enabled);

    cron.enable_job(id, true);
    jobs = cron.list_jobs();
    EXPECT_TRUE(jobs[0].enabled);
}

// ========================================================================
// SchedulerService tests
// ========================================================================

TEST(SchedulerServiceTest, BasicOperations) {
    SchedulerService svc;

    auto id = svc.graph().add_task("SA", []{});
    svc.graph().add_task("SB", []{});

    auto result = svc.run_graph();
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.completed_tasks, 2u);
}

TEST(SchedulerServiceTest, CronIntegration) {
    SchedulerService svc;
    std::atomic<int> count{0};

    auto id = svc.schedule_cron("daily", "0 0 * * *", [&]{ count++; });
    EXPECT_GT(id, 0u);

    auto jobs = svc.cron().list_jobs();
    EXPECT_EQ(jobs.size(), 1u);

    // Start and stop should not throw
    svc.start();
    svc.stop();
    EXPECT_FALSE(svc.cron().is_running());
}

TEST(SchedulerServiceTest, WaveWithDeps) {
    SchedulerService svc;
    std::atomic<int> order{0};
    int a = 0, b = 0;

    auto ta = svc.graph().add_task("A", [&]{ a = ++order; });
    auto tb = svc.graph().add_task("B", [&]{ b = ++order; });
    svc.graph().add_dependency(tb, ta);

    auto result = svc.run_graph();
    EXPECT_TRUE(result.success);
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
}

}  // namespace
}  // namespace quant::scheduler
