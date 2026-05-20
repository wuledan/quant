// factor_dag_from_graph_test.cc — Test FactorDAG::from_graph() IR construction
#include <gtest/gtest.h>

#include <filesystem>

#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"
#include "cpp/quant/factor/op_registry.h"
#include "cpp/quant/ir/ir_graph.h"

using namespace quant::factor;
using namespace quant::ir;

class FactorDAGFromGraphTest : public ::testing::Test {
protected:
    void SetUp() override {
        OpRegistry::register_all_builtin_ops();
    }

    StrategyGraph make_ma_cross_graph() {
        StrategyGraph g;
        g.strategy_name = "ma_cross";

        NodeDef fast_ma;
        fast_ma.id = "fast_ma";
        fast_ma.op_type = "SMA";
        fast_ma.inputs["price"] = {"price", {"TimeSeries", "float"}, "data.close"};
        fast_ma.outputs["value"] = {"value", {"TimeSeries", "float"}, ""};
        fast_ma.params["period"] = 5.0;

        NodeDef slow_ma;
        slow_ma.id = "slow_ma";
        slow_ma.op_type = "SMA";
        slow_ma.inputs["price"] = {"price", {"TimeSeries", "float"}, "data.close"};
        slow_ma.outputs["value"] = {"value", {"TimeSeries", "float"}, ""};
        slow_ma.params["period"] = 20.0;

        NodeDef signal;
        signal.id = "signal";
        signal.op_type = "CROSS_ABOVE";
        signal.inputs["fast"] = {"fast", {"TimeSeries", "float"}, "node.fast_ma.value"};
        signal.inputs["slow"] = {"slow", {"TimeSeries", "float"}, "node.slow_ma.value"};
        signal.outputs["value"] = {"value", {"Signal", "float"}, ""};

        g.nodes = {fast_ma, slow_ma, signal};
        g.edges = {
            {"fast_ma", "value", "signal", "fast"},
            {"slow_ma", "value", "signal", "slow"},
        };
        g.data_bindings = {
            {"kline.close", "fast_ma", "price"},
            {"kline.close", "slow_ma", "price"},
        };

        return g;
    }
};

// ── Test: Build DAG from IR graph ──
TEST_F(FactorDAGFromGraphTest, BuildFromGraph) {
    auto graph = make_ma_cross_graph();
    FactorRegistry registry;
    auto dag = FactorDAG::from_graph(graph, registry);

    ASSERT_NE(dag, nullptr);
    EXPECT_TRUE(dag->is_built());
}

// ── Test: Topological sort respects dependencies ──
TEST_F(FactorDAGFromGraphTest, TopologicalSort) {
    auto graph = make_ma_cross_graph();
    FactorRegistry registry;
    auto dag = FactorDAG::from_graph(graph, registry);

    auto order = dag->topological_sort();
    ASSERT_EQ(order.size(), 3u);

    // fast_ma and slow_ma should come before signal
    FactorId fast_id = registry.find_id("fast_ma");
    FactorId slow_id = registry.find_id("slow_ma");
    FactorId signal_id = registry.find_id("signal");

    size_t fast_pos = 0, slow_pos = 0, signal_pos = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == fast_id) fast_pos = i;
        else if (order[i] == slow_id) slow_pos = i;
        else if (order[i] == signal_id) signal_pos = i;
    }
    EXPECT_LT(fast_pos, signal_pos);
    EXPECT_LT(slow_pos, signal_pos);
}

// ── Test: Parallel levels ──
TEST_F(FactorDAGFromGraphTest, ParallelLevels) {
    auto graph = make_ma_cross_graph();
    FactorRegistry registry;
    auto dag = FactorDAG::from_graph(graph, registry);

    auto levels = dag->parallel_levels();
    ASSERT_EQ(levels.size(), 2u);
    // Level 0: fast_ma + slow_ma (parallel)
    EXPECT_EQ(levels[0].size(), 2u);
    // Level 1: signal (depends on both)
    EXPECT_EQ(levels[1].size(), 1u);
}

// ── Test: Unknown op_type throws ──
TEST_F(FactorDAGFromGraphTest, UnknownOpType) {
    StrategyGraph g;
    g.strategy_name = "bad_op";
    NodeDef n;
    n.id = "bad";
    n.op_type = "NONEXISTENT_OP";
    g.nodes = {n};

    FactorRegistry registry;
    EXPECT_THROW(FactorDAG::from_graph(g, registry), std::runtime_error);
}

// ── Test: Validate DAG ──
TEST_F(FactorDAGFromGraphTest, ValidateDAG) {
    auto graph = make_ma_cross_graph();
    FactorRegistry registry;
    auto dag = FactorDAG::from_graph(graph, registry);

    auto result = dag->validate();
    EXPECT_TRUE(result.valid);
}

// ── Test: Get dependencies ──
TEST_F(FactorDAGFromGraphTest, GetDependencies) {
    auto graph = make_ma_cross_graph();
    FactorRegistry registry;
    auto dag = FactorDAG::from_graph(graph, registry);

    FactorId signal_id = registry.find_id("signal");
    auto deps = dag->get_dependencies(signal_id);
    EXPECT_EQ(deps.size(), 2u);
}

// ── Test: File-based round-trip ──
TEST_F(FactorDAGFromGraphTest, FileRoundTrip) {
    auto graph = make_ma_cross_graph();
    auto test_dir = std::filesystem::temp_directory_path() / "dag_ir_test";
    std::filesystem::create_directories(test_dir);
    auto path = (test_dir / "ma_cross.graph").string();
    graph.write_to_file(path);

    auto loaded = StrategyGraph::load_from_file(path);
    FactorRegistry registry;
    auto dag = FactorDAG::from_graph(loaded, registry);
    ASSERT_NE(dag, nullptr);
    EXPECT_TRUE(dag->is_built());

    std::filesystem::remove_all(test_dir);
}
