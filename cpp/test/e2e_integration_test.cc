// e2e_integration_test.cc — End-to-end integration test:
//   StorageEngine → IR graph → FactorDAG → BacktestRunner → BacktestResult
//
// Validates the complete pipeline from synthetic kline data through
// SMA crossover strategy execution to result verification.
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

#include "cpp/quant/backtest/backtest_runner.h"
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"
#include "cpp/quant/factor/op_registry.h"
#include "cpp/quant/ir/ir_graph.h"
#include "cpp/quant/storage/storage_engine.h"

namespace bt  = quant::backtest;
namespace ev  = quant::event;
namespace ir  = quant::ir;
namespace fac = quant::factor;
namespace sto = quant::storage;

// ── Temp directory helper (same pattern as storage_test.cc) ──
class TempDir {
public:
    TempDir() {
        auto path = std::filesystem::temp_directory_path() / "e2e_test_XXXXXX";
        std::string tmpl = path.string();
        auto* p = mkdtemp(tmpl.data());
        if (p == nullptr) {
            throw std::runtime_error("Failed to create temp directory");
        }
        path_ = tmpl;
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    const std::string& path() const { return path_; }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

private:
    std::string path_;
};

// ── E2E Integration Test Fixture ──
class E2EIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Register all built-in operators (SMA, CROSS_ABOVE, etc.)
        fac::OpRegistry::register_all_builtin_ops();

        // Create StorageEngine with temp directory
        storage_ = std::make_unique<sto::StorageEngine>(
            sto::StorageEngine::Options{4, tmpdir_.path()});

        // Create EventBus
        bus_ = std::make_unique<ev::EventBus>(ev::EventBus::Options{});

        // Generate and store synthetic kline data
        generate_test_data();
    }

    void TearDown() override {
        storage_->close();
        storage_.reset();
        bus_.reset();
        // TempDir destructor cleans up filesystem
    }

    // Generate 50 bars of daily kline data with a clear SMA crossover pattern.
    //   Bars 0-19:  price declining  (slow SMA > fast SMA → no buy signal)
    //   Bars 20-49: price rising     (fast SMA crosses above slow SMA → buy signal)
    void generate_test_data() {
        constexpr int kNumBars = 50;
        constexpr int64_t kDayUs = 86400LL * 1'000'000LL;  // 1 day in microseconds
        constexpr int64_t kBaseTs = 1700000000000000LL;     // arbitrary start

        constexpr double kStartPrice = 100.0;   // 100 yuan
        constexpr double kDeclineStep = 0.5;     // decline 0.5 yuan/day
        constexpr double kRiseStep = 1.0;        // rise 1.0 yuan/day

        std::vector<ev::KlineRow> rows;
        rows.reserve(kNumBars);

        for (int i = 0; i < kNumBars; ++i) {
            double price;
            if (i < 20) {
                // Declining phase: 100 → 90.5
                price = kStartPrice - kDeclineStep * i;
            } else {
                // Rising phase: 90.5 → 110.5
                price = (kStartPrice - kDeclineStep * 19) + kRiseStep * (i - 19);
            }

            ev::KlineRow row{};
            row.timestamp   = kBaseTs + static_cast<int64_t>(i) * kDayUs;
            row.open_price  = static_cast<int32_t>(std::round(price * 10000.0));
            row.high_price  = static_cast<int32_t>(std::round((price + 0.3) * 10000.0));
            row.low_price   = static_cast<int32_t>(std::round((price - 0.3) * 10000.0));
            row.close_price = static_cast<int32_t>(std::round(price * 10000.0));
            row.volume      = 1'000'000 + i * 10'000;
            row.amount      = static_cast<int64_t>(std::round(price * 10000.0)) * 100;
            row.vwap        = static_cast<int32_t>(std::round(price * 10000.0));
            rows.push_back(row);
        }

        storage_->store_kline_batch(
            kSymbol, static_cast<uint8_t>(ev::DataType::kKlineDay), rows);
    }

    // Create an SMA crossover IR graph:
    //   fast_ma (SMA period=5) ← kline.close
    //   slow_ma (SMA period=20) ← kline.close
    //   signal (CROSS_ABOVE) ← fast_ma.value, slow_ma.value
    //   SignalHandler: order (buy_weight=0.95, sell_weight=1.0)
    ir::StrategyGraph create_sma_cross_graph() {
        ir::StrategyGraph g;
        g.strategy_name = "sma_cross_e2e";

        // ── Node: fast_ma ──
        ir::NodeDef fast_ma;
        fast_ma.id = "fast_ma";
        fast_ma.op_type = "SMA";
        fast_ma.inputs["price"] = {"price", {"TimeSeries", "float"}, "data.close"};
        fast_ma.outputs["value"] = {"value", {"TimeSeries", "float"}, ""};
        fast_ma.params["period"] = 5.0;

        // ── Node: slow_ma ──
        ir::NodeDef slow_ma;
        slow_ma.id = "slow_ma";
        slow_ma.op_type = "SMA";
        slow_ma.inputs["price"] = {"price", {"TimeSeries", "float"}, "data.close"};
        slow_ma.outputs["value"] = {"value", {"TimeSeries", "float"}, ""};
        slow_ma.params["period"] = 20.0;

        // ── Node: signal ──
        ir::NodeDef signal;
        signal.id = "signal";
        signal.op_type = "CROSS_ABOVE";
        signal.inputs["fast"] = {"fast", {"TimeSeries", "float"}, "node.fast_ma.value"};
        signal.inputs["slow"] = {"slow", {"TimeSeries", "float"}, "node.slow_ma.value"};
        signal.outputs["value"] = {"value", {"Signal", "float"}, ""};

        g.nodes = {fast_ma, slow_ma, signal};

        // ── Edges ──
        g.edges = {
            {"fast_ma", "value", "signal", "fast"},
            {"slow_ma", "value", "signal", "slow"},
        };

        // ── Data bindings ──
        g.data_bindings = {
            {"kline.close", "fast_ma", "price"},
            {"kline.close", "slow_ma", "price"},
        };

        // ── Signal handler ──
        ir::SignalHandler sh;
        sh.signal_node = "signal";
        sh.handler_type = "order";
        sh.params["buy_weight"] = 0.95;
        sh.params["sell_weight"] = 1.0;
        g.signal_handlers = {sh};

        return g;
    }

    // Helper: build default backtest params matching the test data
    bt::BacktestParams make_params() const {
        bt::BacktestParams params;
        params.initial_cash = 1'000'000.0;
        params.start_time   = 0;
        params.end_time     = 9999999999999999LL;  // wide range to cover all data
        params.symbol       = kSymbol;
        params.kline_type   = ev::DataType::kKlineDay;
        return params;
    }

    // Generate test data for a specific symbol (same price pattern)
    void generate_test_data_for_symbol(const std::string& symbol) {
        constexpr int kNumBars = 50;
        constexpr int64_t kDayUs = 86400LL * 1'000'000LL;
        constexpr int64_t kBaseTs = 1700000000000000LL;

        constexpr double kStartPrice = 100.0;
        constexpr double kDeclineStep = 0.5;
        constexpr double kRiseStep = 1.0;

        std::vector<ev::KlineRow> rows;
        rows.reserve(kNumBars);

        for (int i = 0; i < kNumBars; ++i) {
            double price;
            if (i < 20) {
                price = kStartPrice - kDeclineStep * i;
            } else {
                price = (kStartPrice - kDeclineStep * 19) + kRiseStep * (i - 19);
            }

            ev::KlineRow row{};
            row.timestamp   = kBaseTs + static_cast<int64_t>(i) * kDayUs;
            row.open_price  = static_cast<int32_t>(std::round(price * 10000.0));
            row.high_price  = static_cast<int32_t>(std::round((price + 0.3) * 10000.0));
            row.low_price   = static_cast<int32_t>(std::round((price - 0.3) * 10000.0));
            row.close_price = static_cast<int32_t>(std::round(price * 10000.0));
            row.volume      = 1'000'000 + i * 10'000;
            row.amount      = static_cast<int64_t>(std::round(price * 10000.0)) * 100;
            row.vwap        = static_cast<int32_t>(std::round(price * 10000.0));
            rows.push_back(row);
        }

        storage_->store_kline_batch(
            symbol, static_cast<uint8_t>(ev::DataType::kKlineDay), rows);
    }

    // Helper: build default backtest params for a given symbol
    bt::BacktestParams make_params(const std::string& symbol) const {
        bt::BacktestParams params;
        params.initial_cash = 1'000'000.0;
        params.start_time   = 0;
        params.end_time     = 9999999999999999LL;
        params.symbol       = symbol;
        params.kline_type   = ev::DataType::kKlineDay;
        return params;
    }

    // Test constants
    static constexpr const char* kSymbol = "600519.SH";
    static constexpr const char* kSymbol2 = "000001.SZ";

    // Members
    TempDir tmpdir_;
    std::unique_ptr<sto::StorageEngine> storage_;
    std::unique_ptr<ev::EventBus> bus_;
};

// ─────────────────────────────────────────────────────────────
// Test 1: FullPipeline — data → graph → backtest → valid result
// ─────────────────────────────────────────────────────────────
TEST_F(E2EIntegrationTest, FullPipeline) {
    auto graph = create_sma_cross_graph();
    bt::BacktestRunner runner(*storage_, *bus_);

    auto result = runner.run(graph, make_params());

    // nav_curve should have entries (one per bar)
    EXPECT_FALSE(result.nav_curve.empty());
    // total_trades should be >= 0 (may or may not trade depending on signal)
    EXPECT_GE(result.total_trades, 0);
    // total_return is a finite number
    EXPECT_TRUE(std::isfinite(result.total_return));
    // max_drawdown is between 0 and 1
    EXPECT_GE(result.max_drawdown, 0.0);
    EXPECT_LE(result.max_drawdown, 1.0);
    // sharpe_ratio is finite (could be 0 if no variance)
    EXPECT_TRUE(std::isfinite(result.sharpe_ratio));
}

// ─────────────────────────────────────────────────────────────
// Test 2: GraphFileRoundTrip — write graph → load → run → same result
// ─────────────────────────────────────────────────────────────
TEST_F(E2EIntegrationTest, GraphFileRoundTrip) {
    auto graph = create_sma_cross_graph();

    // Write to file
    auto graph_dir = std::filesystem::temp_directory_path() / "e2e_graph_roundtrip";
    std::filesystem::create_directories(graph_dir);
    auto graph_path = (graph_dir / "sma_cross.graph").string();
    graph.write_to_file(graph_path);

    // Run from in-memory graph
    bt::BacktestRunner runner(*storage_, *bus_);
    auto result_mem = runner.run(graph, make_params());

    // Run from file-loaded graph
    auto result_file = runner.run(graph_path, make_params());

    // Results should be identical
    EXPECT_EQ(result_mem.nav_curve.size(), result_file.nav_curve.size());
    EXPECT_DOUBLE_EQ(result_mem.total_return, result_file.total_return);
    EXPECT_DOUBLE_EQ(result_mem.max_drawdown, result_file.max_drawdown);
    EXPECT_DOUBLE_EQ(result_mem.sharpe_ratio, result_file.sharpe_ratio);
    EXPECT_EQ(result_mem.total_trades, result_file.total_trades);

    // Verify nav curves match
    ASSERT_EQ(result_mem.nav_curve.size(), result_file.nav_curve.size());
    for (size_t i = 0; i < result_mem.nav_curve.size(); ++i) {
        EXPECT_DOUBLE_EQ(result_mem.nav_curve[i].second,
                         result_file.nav_curve[i].second);
    }

    std::filesystem::remove_all(graph_dir);
}

// ─────────────────────────────────────────────────────────────
// Test 3: EmptyDataReturnsEmpty — no kline data → empty result
// ─────────────────────────────────────────────────────────────
TEST_F(E2EIntegrationTest, EmptyDataReturnsEmpty) {
    // Create a separate StorageEngine with no data
    TempDir empty_tmpdir;
    sto::StorageEngine empty_storage(
        sto::StorageEngine::Options{1, empty_tmpdir.path()});

    auto graph = create_sma_cross_graph();
    bt::BacktestRunner runner(empty_storage, *bus_);

    bt::BacktestParams params = make_params();
    params.symbol = "NO_DATA_SYMBOL";

    auto result = runner.run(graph, params);

    // With no data, result should be default-constructed (empty)
    EXPECT_TRUE(result.nav_curve.empty());
    EXPECT_DOUBLE_EQ(result.total_return, 0.0);
    EXPECT_DOUBLE_EQ(result.max_drawdown, 0.0);
    EXPECT_DOUBLE_EQ(result.sharpe_ratio, 0.0);
    EXPECT_EQ(result.total_trades, 0);

    empty_storage.close();
}

// ─────────────────────────────────────────────────────────────
// Test 4: DagValidation — build DAG from graph → validate → valid
// ─────────────────────────────────────────────────────────────
TEST_F(E2EIntegrationTest, DagValidation) {
    auto graph = create_sma_cross_graph();

    fac::FactorRegistry registry;
    auto dag = fac::FactorDAG::from_graph(graph, registry);

    ASSERT_NE(dag, nullptr);
    EXPECT_TRUE(dag->is_built());

    auto validation = dag->validate();
    EXPECT_TRUE(validation.valid);
    // validate() sets message to "DAG is valid" on success
    EXPECT_FALSE(validation.message.empty());

    // Topological sort should have 3 nodes
    auto topo = dag->topological_sort();
    EXPECT_EQ(topo.size(), 3u);

    // Parallel levels: level 0 = {fast_ma, slow_ma}, level 1 = {signal}
    auto levels = dag->parallel_levels();
    ASSERT_EQ(levels.size(), 2u);
    EXPECT_EQ(levels[0].size(), 2u);  // fast_ma + slow_ma
    EXPECT_EQ(levels[1].size(), 1u);  // signal
}

// ─────────────────────────────────────────────────────────────
// Test 5: MultiSymbolBacktest — two symbols, both produce valid results
// ─────────────────────────────────────────────────────────────
TEST_F(E2EIntegrationTest, MultiSymbolBacktest) {
    // Generate data for second symbol
    generate_test_data_for_symbol(kSymbol2);

    auto graph = create_sma_cross_graph();
    bt::BacktestRunner runner(*storage_, *bus_);

    // Run backtest for first symbol (data generated in SetUp)
    auto result1 = runner.run(graph, make_params(kSymbol));
    EXPECT_FALSE(result1.nav_curve.empty());
    EXPECT_TRUE(std::isfinite(result1.total_return));
    EXPECT_GE(result1.max_drawdown, 0.0);
    EXPECT_LE(result1.max_drawdown, 1.0);

    // Run backtest for second symbol
    auto result2 = runner.run(graph, make_params(kSymbol2));
    EXPECT_FALSE(result2.nav_curve.empty());
    EXPECT_TRUE(std::isfinite(result2.total_return));
    EXPECT_GE(result2.max_drawdown, 0.0);
    EXPECT_LE(result2.max_drawdown, 1.0);

    // Both should have the same nav_curve size (same data pattern)
    EXPECT_EQ(result1.nav_curve.size(), result2.nav_curve.size());
}

// ─────────────────────────────────────────────────────────────
// Test 6: SignalHandlerValidation — CROSS_ABOVE signal triggers trades
// ─────────────────────────────────────────────────────────────
TEST_F(E2EIntegrationTest, SignalHandlerValidation) {
    auto graph = create_sma_cross_graph();
    bt::BacktestRunner runner(*storage_, *bus_);

    auto result = runner.run(graph, make_params(kSymbol));

    // With synthetic data: first 20 bars declining, then 30 bars rising,
    // SMA(5) should cross above SMA(20) during the rising phase.
    // This triggers CROSS_ABOVE → buys → total_trades > 0
    EXPECT_GT(result.total_trades, 0);

    // Rising market + buy signal → positive return
    EXPECT_GT(result.total_return, 0.0);

    // nav_curve should have entries
    EXPECT_FALSE(result.nav_curve.empty());

    // Positive return → sharpe_ratio > 0
    EXPECT_GT(result.sharpe_ratio, 0.0);

    // max_drawdown is between 0 and 1
    EXPECT_GE(result.max_drawdown, 0.0);
    EXPECT_LE(result.max_drawdown, 1.0);
}

// ─────────────────────────────────────────────────────────────
// Test 7: InvalidGraphHandling — errors do not crash
// ─────────────────────────────────────────────────────────────
TEST_F(E2EIntegrationTest, InvalidGraphHandling) {
    // ── Sub-test 1: IR graph cycle detection via StrategyGraph::validate()
    {
        ir::StrategyGraph cyclic_graph;
        cyclic_graph.strategy_name = "cyclic_test";

        // Two nodes with valid op_types
        ir::NodeDef node_a;
        node_a.id = "A";
        node_a.op_type = "SMA";
        node_a.inputs["price"] = {"price", {"TimeSeries", "float"}, "data.close"};
        node_a.outputs["value"] = {"value", {"TimeSeries", "float"}, ""};
        node_a.params["period"] = 5.0;

        ir::NodeDef node_b;
        node_b.id = "B";
        node_b.op_type = "SMA";
        node_b.inputs["price"] = {"price", {"TimeSeries", "float"}, "data.close"};
        node_b.outputs["value"] = {"value", {"TimeSeries", "float"}, ""};
        node_b.params["period"] = 10.0;

        cyclic_graph.nodes = {node_a, node_b};
        // Edges: A → B → A (cycle)
        cyclic_graph.edges = {
            {"A", "value", "B", "price"},
            {"B", "value", "A", "price"},
        };

        std::string error_msg;
        EXPECT_FALSE(cyclic_graph.validate(error_msg));
        EXPECT_TRUE(error_msg.find("Cycle") != std::string::npos ||
                    error_msg.find("cycle") != std::string::npos);
    }

    // ── Sub-test 2: Unknown op_type → from_graph throws
    {
        ir::StrategyGraph bad_graph;
        bad_graph.strategy_name = "unknown_op_test";

        ir::NodeDef node;
        node.id = "bad_node";
        node.op_type = "UNKNOWN_OP_XYZ";  // not registered
        node.inputs["price"] = {"price", {"TimeSeries", "float"}, "data.close"};
        node.outputs["value"] = {"value", {"TimeSeries", "float"}, ""};

        bad_graph.nodes = {node};

        fac::FactorRegistry registry;
        EXPECT_THROW(
            fac::FactorDAG::from_graph(bad_graph, registry),
            std::runtime_error);
    }
}
