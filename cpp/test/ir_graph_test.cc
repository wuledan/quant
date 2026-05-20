// ir_graph_test.cc — Unit tests for StrategyGraph IR
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

#include "cpp/quant/ir/ir_graph.h"

using namespace quant::ir;

class IRGraphTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "ir_test";
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }

    StrategyGraph make_ma_cross_graph() {
        StrategyGraph g;
        g.strategy_name = "ma_cross";
        g.version = 1;

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

        g.signal_handlers = {
            {"signal", "order", {{"buy_weight", 0.95}, {"sell_weight", 1.0}}},
        };

        return g;
    }

    std::filesystem::path test_dir_;
};

// ── Test: JSON round-trip ──
TEST_F(IRGraphTest, JsonRoundTrip) {
    auto g = make_ma_cross_graph();
    auto json = g.to_json();

    auto g2 = StrategyGraph::load_from_json(json);
    EXPECT_EQ(g2.strategy_name, "ma_cross");
    EXPECT_EQ(g2.version, 1u);
    EXPECT_EQ(g2.nodes.size(), 3u);
    EXPECT_EQ(g2.edges.size(), 2u);
    EXPECT_EQ(g2.data_bindings.size(), 2u);
    EXPECT_EQ(g2.signal_handlers.size(), 1u);
}

// ── Test: File round-trip ──
TEST_F(IRGraphTest, FileRoundTrip) {
    auto g = make_ma_cross_graph();
    auto path = (test_dir_ / "ma_cross.graph").string();
    g.write_to_file(path);

    auto g2 = StrategyGraph::load_from_file(path);
    EXPECT_EQ(g2.strategy_name, "ma_cross");
    EXPECT_EQ(g2.nodes.size(), 3u);

    // Verify node details
    auto* fast = g2.find_node("fast_ma");
    ASSERT_NE(fast, nullptr);
    EXPECT_EQ(fast->op_type, "SMA");
    EXPECT_EQ(fast->params.at("period"), 5.0);
}

// ── Test: Validate correct graph ──
TEST_F(IRGraphTest, ValidateCorrectGraph) {
    auto g = make_ma_cross_graph();
    std::string err;
    EXPECT_TRUE(g.validate(err)) << err;
}

// ── Test: Detect duplicate node IDs ──
TEST_F(IRGraphTest, DetectDuplicateNodeIds) {
    StrategyGraph g;
    g.strategy_name = "dup_test";
    NodeDef n1;
    n1.id = "a";
    n1.op_type = "SMA";
    NodeDef n2;
    n2.id = "a";
    n2.op_type = "EMA";
    g.nodes = {n1, n2};

    std::string err;
    EXPECT_FALSE(g.validate(err));
    EXPECT_TRUE(err.find("Duplicate") != std::string::npos);
}

// ── Test: Detect cycle ──
TEST_F(IRGraphTest, DetectCycle) {
    StrategyGraph g;
    g.strategy_name = "cycle_test";
    NodeDef a, b, c;
    a.id = "a"; a.op_type = "SMA";
    b.id = "b"; b.op_type = "SMA";
    c.id = "c"; c.op_type = "SMA";
    g.nodes = {a, b, c};
    g.edges = {{"a", "value", "b", "input"}, {"b", "value", "c", "input"}, {"c", "value", "a", "input"}};

    std::string err;
    EXPECT_FALSE(g.validate(err));
    EXPECT_TRUE(err.find("Cycle") != std::string::npos);
}

// ── Test: Edge references non-existent node ──
TEST_F(IRGraphTest, EdgeInvalidNode) {
    StrategyGraph g;
    g.strategy_name = "bad_edge";
    NodeDef a;
    a.id = "a"; a.op_type = "SMA";
    g.nodes = {a};
    g.edges = {{"a", "value", "nonexistent", "input"}};

    std::string err;
    EXPECT_FALSE(g.validate(err));
    EXPECT_TRUE(err.find("non-existent") != std::string::npos);
}

// ── Test: find_node ──
TEST_F(IRGraphTest, FindNode) {
    auto g = make_ma_cross_graph();
    ASSERT_NE(g.find_node("fast_ma"), nullptr);
    ASSERT_NE(g.find_node("slow_ma"), nullptr);
    ASSERT_NE(g.find_node("signal"), nullptr);
    EXPECT_EQ(g.find_node("nonexistent"), nullptr);
}

// ── Test: nodes_by_op ──
TEST_F(IRGraphTest, NodesByOp) {
    auto g = make_ma_cross_graph();
    auto sma_nodes = g.nodes_by_op("SMA");
    EXPECT_EQ(sma_nodes.size(), 2u);

    auto cross_nodes = g.nodes_by_op("CROSS_ABOVE");
    EXPECT_EQ(cross_nodes.size(), 1u);

    auto empty = g.nodes_by_op("NONEXISTENT");
    EXPECT_EQ(empty.size(), 0u);
}

// ── Test: Signal handler params preserved ──
TEST_F(IRGraphTest, SignalHandlerParams) {
    auto g = make_ma_cross_graph();
    auto json = g.to_json();
    auto g2 = StrategyGraph::load_from_json(json);

    ASSERT_EQ(g2.signal_handlers.size(), 1u);
    EXPECT_EQ(g2.signal_handlers[0].signal_node, "signal");
    EXPECT_EQ(g2.signal_handlers[0].handler_type, "order");
    EXPECT_DOUBLE_EQ(g2.signal_handlers[0].params.at("buy_weight"), 0.95);
    EXPECT_DOUBLE_EQ(g2.signal_handlers[0].params.at("sell_weight"), 1.0);
}

// ── Test: Empty graph ──
TEST_F(IRGraphTest, EmptyGraph) {
    StrategyGraph g;
    g.strategy_name = "empty";
    auto json = g.to_json();
    auto g2 = StrategyGraph::load_from_json(json);
    EXPECT_EQ(g2.nodes.size(), 0u);
    EXPECT_EQ(g2.edges.size(), 0u);
    std::string err;
    EXPECT_TRUE(g2.validate(err));
}

// ── Test: Data binding references non-existent node ──
TEST_F(IRGraphTest, DataBindingInvalidNode) {
    StrategyGraph g;
    g.strategy_name = "bad_binding";
    NodeDef a;
    a.id = "a"; a.op_type = "SMA";
    g.nodes = {a};
    g.data_bindings = {{"kline.close", "nonexistent", "price"}};

    std::string err;
    EXPECT_FALSE(g.validate(err));
    EXPECT_TRUE(err.find("non-existent") != std::string::npos);
}
