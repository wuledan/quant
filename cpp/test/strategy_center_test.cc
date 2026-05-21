// strategy_center_test.cc — Tests for StrategyRegistry and StrategyEngine
#include <gtest/gtest.h>

#include <filesystem>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/factor/op_registry.h"
#include "cpp/quant/ir/ir_graph.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/strategy/signal_handler.h"
#include "cpp/quant/strategy/strategy_engine.h"
#include "cpp/quant/strategy/strategy_registry.h"

namespace strat = quant::strategy;
namespace ev = quant::event;

// ── StrategyRegistry tests ──

TEST(StrategyRegistryTest, RegisterAndFind) {
    strat::StrategyRegistry reg;
    auto id = reg.register_strategy("ma_cross", "/tmp/test.graph");
    ASSERT_NE(id, 0u);

    auto* entry = reg.find(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->name, "ma_cross");
    EXPECT_EQ(entry->graph_path, "/tmp/test.graph");
    EXPECT_EQ(entry->status, strat::StrategyStatus::kDraft);
}

TEST(StrategyRegistryTest, FindByName) {
    strat::StrategyRegistry reg;
    auto id = reg.register_strategy("rsi_strategy", "/tmp/rsi.graph");

    auto* entry = reg.find_by_name("rsi_strategy");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->id, id);
}

TEST(StrategyRegistryTest, FindByNameNotFound) {
    strat::StrategyRegistry reg;
    EXPECT_EQ(reg.find_by_name("nonexistent"), nullptr);
}

TEST(StrategyRegistryTest, UpdateStatus) {
    strat::StrategyRegistry reg;
    auto id = reg.register_strategy("test", "/tmp/test.graph");

    EXPECT_TRUE(reg.update_status(id, strat::StrategyStatus::kActive));
    auto* entry = reg.find(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->status, strat::StrategyStatus::kActive);
}

TEST(StrategyRegistryTest, UpdateGraphPath) {
    strat::StrategyRegistry reg;
    auto id = reg.register_strategy("test", "/tmp/old.graph");

    EXPECT_TRUE(reg.update_graph_path(id, "/tmp/new.graph"));
    auto* entry = reg.find(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->graph_path, "/tmp/new.graph");
}

TEST(StrategyRegistryTest, UpdateParams) {
    strat::StrategyRegistry reg;
    auto id = reg.register_strategy("test", "/tmp/test.graph", {{"period", 20.0}});

    EXPECT_TRUE(reg.update_params(id, {{"period", 30.0}}));
    auto* entry = reg.find(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_DOUBLE_EQ(entry->params.at("period"), 30.0);
}

TEST(StrategyRegistryTest, ListStrategies) {
    strat::StrategyRegistry reg;
    reg.register_strategy("s1", "/tmp/s1.graph");
    reg.register_strategy("s2", "/tmp/s2.graph");

    auto list = reg.list_strategies();
    EXPECT_EQ(list.size(), 2u);
}

TEST(StrategyRegistryTest, ListByStatus) {
    strat::StrategyRegistry reg;
    auto id1 = reg.register_strategy("s1", "/tmp/s1.graph");
    reg.register_strategy("s2", "/tmp/s2.graph");
    reg.update_status(id1, strat::StrategyStatus::kActive);

    auto active = reg.list_by_status(strat::StrategyStatus::kActive);
    EXPECT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].name, "s1");
}

TEST(StrategyRegistryTest, RemoveStrategy) {
    strat::StrategyRegistry reg;
    auto id = reg.register_strategy("test", "/tmp/test.graph");

    EXPECT_TRUE(reg.remove_strategy(id));
    EXPECT_EQ(reg.find(id), nullptr);
    EXPECT_EQ(reg.find_by_name("test"), nullptr);
    EXPECT_EQ(reg.size(), 0u);
}

TEST(StrategyRegistryTest, RemoveNonexistent) {
    strat::StrategyRegistry reg;
    EXPECT_FALSE(reg.remove_strategy(999));
}

TEST(StrategyRegistryTest, UpdateNonexistent) {
    strat::StrategyRegistry reg;
    EXPECT_FALSE(reg.update_status(999, strat::StrategyStatus::kActive));
    EXPECT_FALSE(reg.update_graph_path(999, "/tmp/new.graph"));
}

// ── StrategyEngine tests ──

class StrategyEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        bus_ = std::make_unique<ev::EventBus>(ev::EventBus::Options{});
        storage_ = std::make_unique<quant::storage::StorageEngine>();
        engine_ = std::make_unique<strat::StrategyEngine>(*bus_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        bus_.reset();
    }

    // Create a minimal .graph file for testing
    std::string create_test_graph() {
        auto test_dir = std::filesystem::temp_directory_path() / "strategy_engine_test";
        std::filesystem::create_directories(test_dir);
        auto path = (test_dir / "test.graph").string();

        quant::ir::StrategyGraph g;
        g.strategy_name = "test_strategy";

        quant::ir::NodeDef sma;
        sma.id = "sma5";
        sma.op_type = "SMA";
        sma.inputs["price"] = {"price", {"TimeSeries", "float"}, "data.close"};
        sma.outputs["value"] = {"value", {"TimeSeries", "float"}, ""};
        sma.params["period"] = 5.0;

        g.nodes = {sma};
        g.write_to_file(path);
        return path;
    }

    std::unique_ptr<ev::EventBus> bus_;
    std::unique_ptr<quant::storage::StorageEngine> storage_;
    std::unique_ptr<strat::StrategyEngine> engine_;
};

TEST_F(StrategyEngineTest, ActivateStrategy) {
    auto graph_path = create_test_graph();
    auto id = engine_->registry().register_strategy("test", graph_path);

    EXPECT_TRUE(engine_->activate(id));
    EXPECT_TRUE(engine_->is_active(id));

    auto* entry = engine_->registry().find(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->status, strat::StrategyStatus::kActive);
}

TEST_F(StrategyEngineTest, ActivateNonexistent) {
    EXPECT_FALSE(engine_->activate(999));
}

TEST_F(StrategyEngineTest, PauseAndResume) {
    auto graph_path = create_test_graph();
    auto id = engine_->registry().register_strategy("test", graph_path);
    engine_->activate(id);

    EXPECT_TRUE(engine_->pause(id));
    auto* entry = engine_->registry().find(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->status, strat::StrategyStatus::kPaused);

    EXPECT_TRUE(engine_->resume(id));
    entry = engine_->registry().find(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->status, strat::StrategyStatus::kActive);
}

TEST_F(StrategyEngineTest, DeactivateStrategy) {
    auto graph_path = create_test_graph();
    auto id = engine_->registry().register_strategy("test", graph_path);
    engine_->activate(id);

    EXPECT_TRUE(engine_->deactivate(id));
    EXPECT_FALSE(engine_->is_active(id));

    auto* entry = engine_->registry().find(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->status, strat::StrategyStatus::kDraft);
}

TEST_F(StrategyEngineTest, DeactivateNonexistent) {
    EXPECT_FALSE(engine_->deactivate(999));
}

TEST_F(StrategyEngineTest, PauseNotActive) {
    EXPECT_FALSE(engine_->pause(999));
}

TEST_F(StrategyEngineTest, DoubleActivate) {
    auto graph_path = create_test_graph();
    auto id = engine_->registry().register_strategy("test", graph_path);

    EXPECT_TRUE(engine_->activate(id));
    EXPECT_FALSE(engine_->activate(id));  // already active
}
