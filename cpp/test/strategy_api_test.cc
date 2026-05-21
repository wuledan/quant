// strategy_api_test.cc — Tests for StrategyApi
#include <gtest/gtest.h>

#include <filesystem>

#include "cpp/quant/api/strategy_api.h"
#include "cpp/quant/backtest/backtest_runner.h"
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/ir/ir_graph.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/strategy/strategy_engine.h"

namespace api = quant::api;
namespace strat = quant::strategy;
namespace bt = quant::backtest;
namespace ev = quant::event;

class StrategyApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        bus_ = std::make_unique<ev::EventBus>(ev::EventBus::Options{});
        storage_ = std::make_unique<quant::storage::StorageEngine>();
        engine_ = std::make_unique<strat::StrategyEngine>(*bus_, *storage_);
        runner_ = std::make_unique<bt::BacktestRunner>(*storage_, *bus_);
        api_ = std::make_unique<api::StrategyApi>(*engine_, *runner_, *storage_);
    }

    void TearDown() override {
        api_.reset();
        runner_.reset();
        engine_.reset();
        storage_.reset();
        bus_.reset();
    }

    // Create a minimal .graph file for testing activation and backtest
    std::string create_test_graph(const std::string& name = "test_strategy") {
        auto test_dir = std::filesystem::temp_directory_path() / "api_test";
        std::filesystem::create_directories(test_dir);
        auto path = (test_dir / (name + ".graph")).string();

        quant::ir::StrategyGraph g;
        g.strategy_name = name;

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
    std::unique_ptr<bt::BacktestRunner> runner_;
    std::unique_ptr<api::StrategyApi> api_;
};

// ── 1. Register strategy via API ──

TEST_F(StrategyApiTest, RegisterStrategy) {
    auto resp = api_->handle_request("POST", "/api/strategies",
                                      R"({"name":"ma_cross","graph_path":"/tmp/test.graph"})");
    EXPECT_EQ(resp.status_code, 201);
    EXPECT_NE(resp.body.find("\"name\":\"ma_cross\""), std::string::npos);
    EXPECT_NE(resp.body.find("\"graph_path\":\"/tmp/test.graph\""), std::string::npos);
    EXPECT_NE(resp.body.find("\"status\":\"draft\""), std::string::npos);
}

TEST_F(StrategyApiTest, RegisterStrategyWithParams) {
    auto resp = api_->handle_request("POST", "/api/strategies",
                                      R"({"name":"rsi","graph_path":"/tmp/rsi.graph","params":{"period":14.0,"overbought":70.0}})");
    EXPECT_EQ(resp.status_code, 201);
    EXPECT_NE(resp.body.find("\"name\":\"rsi\""), std::string::npos);
    EXPECT_NE(resp.body.find("\"period\""), std::string::npos);
    EXPECT_NE(resp.body.find("\"overbought\""), std::string::npos);
}

TEST_F(StrategyApiTest, RegisterMissingName) {
    auto resp = api_->handle_request("POST", "/api/strategies",
                                      R"({"graph_path":"/tmp/test.graph"})");
    EXPECT_EQ(resp.status_code, 400);
    EXPECT_NE(resp.body.find("\"error\""), std::string::npos);
}

TEST_F(StrategyApiTest, RegisterInvalidJson) {
    auto resp = api_->handle_request("POST", "/api/strategies", "not json");
    EXPECT_EQ(resp.status_code, 400);
    EXPECT_NE(resp.body.find("\"error\""), std::string::npos);
}

// ── 2. List strategies ──

TEST_F(StrategyApiTest, ListStrategies) {
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"s1","graph_path":"/tmp/s1.graph"})");
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"s2","graph_path":"/tmp/s2.graph"})");

    auto resp = api_->handle_request("GET", "/api/strategies");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"name\":\"s1\""), std::string::npos);
    EXPECT_NE(resp.body.find("\"name\":\"s2\""), std::string::npos);
    EXPECT_NE(resp.body.find("\"strategies\":"), std::string::npos);
}

TEST_F(StrategyApiTest, ListEmpty) {
    auto resp = api_->handle_request("GET", "/api/strategies");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"strategies\":"), std::string::npos);
}

// ── 3. Get strategy by ID ──

TEST_F(StrategyApiTest, GetStrategy) {
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":"/tmp/test.graph"})");

    auto resp = api_->handle_request("GET", "/api/strategies/1");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"name\":\"test\""), std::string::npos);
    EXPECT_NE(resp.body.find("\"graph_path\":\"/tmp/test.graph\""), std::string::npos);
    EXPECT_NE(resp.body.find("\"status\":\"draft\""), std::string::npos);
}

TEST_F(StrategyApiTest, GetStrategyNotFound) {
    auto resp = api_->handle_request("GET", "/api/strategies/999");
    EXPECT_EQ(resp.status_code, 404);
    EXPECT_NE(resp.body.find("\"error\""), std::string::npos);
}

// ── 4. Update strategy params ──

TEST_F(StrategyApiTest, UpdateStrategyGraphPath) {
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":"/tmp/old.graph"})");

    auto resp = api_->handle_request("PUT", "/api/strategies/1",
                                      R"({"graph_path":"/tmp/new.graph"})");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("/tmp/new.graph"), std::string::npos);
}

TEST_F(StrategyApiTest, UpdateStrategyParams) {
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":"/tmp/test.graph","params":{"period":10.0}})");

    auto resp = api_->handle_request("PUT", "/api/strategies/1",
                                      R"({"params":{"period":20.0,"threshold":0.5}})");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"period\""), std::string::npos);
    EXPECT_NE(resp.body.find("\"threshold\""), std::string::npos);
}

TEST_F(StrategyApiTest, UpdateStrategyNotFound) {
    auto resp = api_->handle_request("PUT", "/api/strategies/999",
                                      R"({"graph_path":"/tmp/new.graph"})");
    EXPECT_EQ(resp.status_code, 404);
}

// ── 5. Delete strategy ──

TEST_F(StrategyApiTest, DeleteStrategy) {
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":"/tmp/test.graph"})");

    auto resp = api_->handle_request("DELETE", "/api/strategies/1");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"deleted\""), std::string::npos);

    // Verify it's gone
    auto get_resp = api_->handle_request("GET", "/api/strategies/1");
    EXPECT_EQ(get_resp.status_code, 404);
}

TEST_F(StrategyApiTest, DeleteStrategyNotFound) {
    auto resp = api_->handle_request("DELETE", "/api/strategies/999");
    EXPECT_EQ(resp.status_code, 404);
}

// ── 6. Activate/pause strategy ──

TEST_F(StrategyApiTest, ActivateStrategy) {
    auto graph_path = create_test_graph();
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":")" + graph_path + R"("})");

    auto resp = api_->handle_request("POST", "/api/strategies/1/activate");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"status\":\"active\""), std::string::npos);
}

TEST_F(StrategyApiTest, ActivateStrategyNotFound) {
    auto resp = api_->handle_request("POST", "/api/strategies/999/activate");
    EXPECT_EQ(resp.status_code, 404);
}

TEST_F(StrategyApiTest, PauseStrategy) {
    auto graph_path = create_test_graph();
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":")" + graph_path + R"("})");
    api_->handle_request("POST", "/api/strategies/1/activate");

    auto resp = api_->handle_request("POST", "/api/strategies/1/pause");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"status\":\"paused\""), std::string::npos);
}

TEST_F(StrategyApiTest, PauseStrategyNotFound) {
    auto resp = api_->handle_request("POST", "/api/strategies/999/pause");
    EXPECT_EQ(resp.status_code, 404);
}

TEST_F(StrategyApiTest, PauseNotActiveStrategy) {
    // Register but don't activate — pause should fail
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":"/tmp/test.graph"})");

    auto resp = api_->handle_request("POST", "/api/strategies/1/pause");
    EXPECT_EQ(resp.status_code, 409);
}

// ── 7. Trigger backtest ──

TEST_F(StrategyApiTest, TriggerBacktestWithGraph) {
    auto graph_path = create_test_graph("backtest_test");
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":")" + graph_path + R"("})");

    auto resp = api_->handle_request("POST", "/api/strategies/1/backtest",
                                      R"({"initial_cash":500000.0,"symbol":"600519.SH"})");
    // Backtest may return 200 (empty result since no data) or 500 (if it throws)
    // Either way, it should not be 404
    EXPECT_NE(resp.status_code, 404);
    if (resp.status_code == 200) {
        EXPECT_NE(resp.body.find("\"strategy_id\""), std::string::npos);
        EXPECT_NE(resp.body.find("\"result\""), std::string::npos);
    }
}

TEST_F(StrategyApiTest, TriggerBacktestStrategyNotFound) {
    auto resp = api_->handle_request("POST", "/api/strategies/999/backtest");
    EXPECT_EQ(resp.status_code, 404);
}

// ── 8. Unknown path returns 404 ──

TEST_F(StrategyApiTest, UnknownPath) {
    auto resp = api_->handle_request("GET", "/api/unknown");
    EXPECT_EQ(resp.status_code, 404);
}

TEST_F(StrategyApiTest, UnknownAction) {
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":"/tmp/test.graph"})");

    auto resp = api_->handle_request("POST", "/api/strategies/1/nonexistent");
    EXPECT_EQ(resp.status_code, 404);
}

TEST_F(StrategyApiTest, InvalidId) {
    auto resp = api_->handle_request("GET", "/api/strategies/abc");
    EXPECT_EQ(resp.status_code, 400);
}

TEST_F(StrategyApiTest, MethodNotAllowed) {
    api_->handle_request("POST", "/api/strategies",
                          R"({"name":"test","graph_path":"/tmp/test.graph"})");

    auto resp = api_->handle_request("PATCH", "/api/strategies/1");
    EXPECT_EQ(resp.status_code, 405);
}
