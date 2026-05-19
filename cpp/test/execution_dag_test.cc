// execution_dag_test.cc — Tests for ExecutionDAG
#include "cpp/quant/scheduler/execution_dag.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace quant::scheduler;
using namespace quant::infra;

// ─── Utilities ───

// Returns an executor with a fixed number of workers
static WorkStealingExecutor make_executor(size_t workers = 4) {
    return WorkStealingExecutor(workers, "dag_test");
}

// ─── ExecutionDAGTest ───

TEST(ExecutionDAGTest, EmptyDAG) {
    auto executor = make_executor();
    executor.start();

    ExecutionDAG dag;
    // Use blockingWait directly instead of execute()
    auto result = blockingWait(dag.co_execute(executor));
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.error.empty());

    executor.stop();
}

TEST(ExecutionDAGTest, LinearChain) {
    auto executor = make_executor();
    executor.start();

    std::vector<int> order;
    std::mutex order_mutex;

    ExecutionDAG dag;
    auto id_a = dag.add_task("A", [&]() -> folly::coro::Task<void> {
        std::lock_guard lk(order_mutex);
        order.push_back(1);
        co_return;
    });
    auto id_b = dag.add_task("B", [&]() -> folly::coro::Task<void> {
        std::lock_guard lk(order_mutex);
        order.push_back(2);
        co_return;
    });
    auto id_c = dag.add_task("C", [&]() -> folly::coro::Task<void> {
        std::lock_guard lk(order_mutex);
        order.push_back(3);
        co_return;
    });

    // A → B → C
    ASSERT_TRUE(dag.add_dependency(id_b, id_a));
    ASSERT_TRUE(dag.add_dependency(id_c, id_b));

    auto result = blockingWait(dag.co_execute(executor));
    EXPECT_TRUE(result.success) << result.error;

    // Verify order: each level executes one task
    ASSERT_EQ(order.size(), 3);
    EXPECT_EQ(order[0], 1);  // A
    EXPECT_EQ(order[1], 2);  // B
    EXPECT_EQ(order[2], 3);  // C

    executor.stop();
}

TEST(ExecutionDAGTest, ParallelExecution) {
    auto executor = make_executor();
    executor.start();

    std::atomic<int> counter{0};

    ExecutionDAG dag;
    for (int i = 0; i < 4; ++i) {
        dag.add_task("Parallel_" + std::to_string(i),
                     [&counter]() -> folly::coro::Task<void> {
                         counter.fetch_add(1, std::memory_order_relaxed);
                         co_return;
                     });
    }

    // No dependencies — all in one parallel level
    auto levels = dag.parallel_levels();
    ASSERT_EQ(levels.size(), 1);
    EXPECT_EQ(levels[0].size(), 4);

    auto result = blockingWait(dag.co_execute(executor));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(counter.load(), 4);

    executor.stop();
}

TEST(ExecutionDAGTest, DagLayerOrder) {
    auto executor = make_executor(2);
    executor.start();

    // Track whether each layer executed
    std::atomic<bool> layer0_done{false};
    std::atomic<bool> layer1_check_passed{true};

    ExecutionDAG dag;

    // Layer 0: two independent tasks
    auto id_a = dag.add_task("Layer0_A", [&layer0_done]() -> folly::coro::Task<void> {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        layer0_done.store(true, std::memory_order_release);
        co_return;
    });
    auto id_b = dag.add_task("Layer0_B", [&layer0_done]() -> folly::coro::Task<void> {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        layer0_done.store(true, std::memory_order_release);
        co_return;
    });

    // Layer 1: depends on both layer 0 tasks
    auto id_c = dag.add_task("Layer1_C", [&layer0_done, &layer1_check_passed]() -> folly::coro::Task<void> {
        // Layer 0 must be done before Layer 1
        bool l0 = layer0_done.load(std::memory_order_acquire);
        if (!l0) {
            layer1_check_passed.store(false, std::memory_order_relaxed);
        }
        co_return;
    });

    ASSERT_TRUE(dag.add_dependency(id_c, id_a));
    ASSERT_TRUE(dag.add_dependency(id_c, id_b));

    auto levels = dag.parallel_levels();
    ASSERT_EQ(levels.size(), 2);
    EXPECT_EQ(levels[0].size(), 2);  // Layer 0: A, B
    EXPECT_EQ(levels[1].size(), 1);  // Layer 1: C

    auto result = blockingWait(dag.co_execute(executor));
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_TRUE(layer1_check_passed.load());

    executor.stop();
}

TEST(ExecutionDAGTest, CycleDetection) {
    ExecutionDAG dag;

    auto id_a = dag.add_task("A", []() -> folly::coro::Task<void> { co_return; });
    auto id_b = dag.add_task("B", []() -> folly::coro::Task<void> { co_return; });
    auto id_c = dag.add_task("C", []() -> folly::coro::Task<void> { co_return; });

    // A → B → C → A (cycle)
    ASSERT_TRUE(dag.add_dependency(id_b, id_a));
    ASSERT_TRUE(dag.add_dependency(id_c, id_b));
    ASSERT_TRUE(dag.add_dependency(id_a, id_c));

    auto vr = dag.validate();
    EXPECT_FALSE(vr.ok);

    // Also verify execute returns failure via validation
    auto executor = make_executor();
    executor.start();
    auto result = blockingWait(dag.co_execute(executor));
    EXPECT_FALSE(result.success);
    executor.stop();
}

TEST(ExecutionDAGTest, LargeDAG) {
    constexpr size_t kNodeCount = 50;
    auto executor = make_executor();
    executor.start();

    ExecutionDAG dag;
    std::atomic<size_t> exec_count{0};

    // Build a binary-tree DAG: each node depends on its parent
    // Node 0 is root, node i depends on (i-1)/2
    std::vector<TaskId> ids;
    ids.reserve(kNodeCount);
    for (size_t i = 0; i < kNodeCount; ++i) {
        ids.push_back(dag.add_task(
            "Node_" + std::to_string(i),
            [&exec_count]() -> folly::coro::Task<void> {
                exec_count.fetch_add(1, std::memory_order_relaxed);
                co_return;
            }));
    }

    // Add dependencies forming a DAG (not a tree, to allow parallelism)
    // Node 0 root, each subsequent node depends on one earlier node
    for (size_t i = 1; i < kNodeCount; ++i) {
        ASSERT_TRUE(dag.add_dependency(ids[i], ids[(i - 1) / 2]));
    }

    auto vr = dag.validate();
    ASSERT_TRUE(vr.ok) << vr.error;

    EXPECT_EQ(dag.size(), kNodeCount);

    auto result = blockingWait(dag.co_execute(executor));
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_EQ(exec_count.load(), kNodeCount);

    executor.stop();
}

TEST(ExecutionDAGTest, FailedTask) {
    auto executor = make_executor();
    executor.start();

    ExecutionDAG dag;
    dag.add_task("GoodTask", []() -> folly::coro::Task<void> {
        co_return;
    });
    dag.add_task("BadTask", []() -> folly::coro::Task<void> {
        throw std::runtime_error("intentional failure");
        co_return;
    });

    auto result = blockingWait(dag.co_execute(executor));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
    EXPECT_TRUE(result.error.find("BadTask") != std::string::npos);
    EXPECT_TRUE(result.error.find("intentional failure") != std::string::npos);

    // Also check that total_time_ms is set
    EXPECT_GT(result.total_time_ms, 0);

    executor.stop();
}

