// test_ws_event_bridge.cc — Unit tests for WsEventBridge
//
// Tests JSON serialization for all event types and the bridge lifecycle.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/event/events/factor_update_event.h"
#include "cpp/quant/event/events/order_report_event.h"
#include "cpp/quant/event/events/risk_alert_event.h"
#include "cpp/quant/event/events/trade_signal_event.h"
#include "cpp/quant/event/events/market_data_event.h"
#include "cpp/quant/network/ws_event_bridge.h"
#include "cpp/quant/network/websocket_server.h"

using namespace quant;

// ── Test fixture ──
class WsEventBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        event::EventBus::Options bus_opts;
        bus_ = std::make_unique<event::EventBus>(bus_opts);
        bus_->start();

        network::WsServerConfig ws_config;
        ws_config.port = 0;  // Don't actually bind a port for testing
        ws_server_ = std::make_unique<network::WebSocketServer>(ws_config);
    }

    void TearDown() override {
        if (bridge_) bridge_->stop();
        bus_->stop();
        ws_server_->stop();
    }

    std::unique_ptr<event::EventBus> bus_;
    std::unique_ptr<network::WebSocketServer> ws_server_;
    std::unique_ptr<network::WsEventBridge> bridge_;
};

// ── Test: Serialize KlineEvent ──
TEST_F(WsEventBridgeTest, SerializeKlineEvent) {
    bridge_ = std::make_unique<network::WsEventBridge>(*bus_, *ws_server_);

    event::KlineEvent kline_evt;
    kline_evt.symbol = "600519.SH";
    kline_evt.kline_type = event::DataType::kKlineDay;
    kline_evt.kline.timestamp = 1700000000000;
    kline_evt.kline.open_price = 34005000;   // 3400.50 * 10000
    kline_evt.kline.high_price = 34200000;   // 3420.00 * 10000
    kline_evt.kline.low_price  = 33802500;   // 3380.25 * 10000
    kline_evt.kline.close_price = 34107500;  // 3410.75 * 10000
    kline_evt.kline.volume = 1500000;
    kline_evt.kline.amount = 5125000000;
    kline_evt.kline.vwap = 0;

    std::string json = bridge_->serialize_event(kline_evt);

    // Verify envelope structure
    EXPECT_TRUE(json.find("\"channel\":\"kline\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"data\":") != std::string::npos);
    EXPECT_TRUE(json.find("\"ts\":") != std::string::npos);

    // Verify kline fields
    EXPECT_TRUE(json.find("\"symbol\":\"600519.SH\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"open\":3400.50") != std::string::npos);
    EXPECT_TRUE(json.find("\"high\":3420.00") != std::string::npos);
    EXPECT_TRUE(json.find("\"low\":3380.25") != std::string::npos);
    EXPECT_TRUE(json.find("\"close\":3410.75") != std::string::npos);
    EXPECT_TRUE(json.find("\"volume\":1500000") != std::string::npos);
}

// ── Test: Serialize FactorUpdateEvent ──
TEST_F(WsEventBridgeTest, SerializeFactorEvent) {
    bridge_ = std::make_unique<network::WsEventBridge>(*bus_, *ws_server_);

    event::FactorUpdateEvent factor_evt;
    factor_evt.factor_id = 42;
    factor_evt.trading_day = 20240101;

    std::string json = bridge_->serialize_event(factor_evt);

    EXPECT_TRUE(json.find("\"channel\":\"factor\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"factor_id\":42") != std::string::npos);
    EXPECT_TRUE(json.find("\"trading_day\":20240101") != std::string::npos);
}

// ── Test: Serialize OrderReportEvent ──
TEST_F(WsEventBridgeTest, SerializeOrderEvent) {
    bridge_ = std::make_unique<network::WsEventBridge>(*bus_, *ws_server_);

    event::OrderReportEvent order_evt;
    order_evt.order_id.internal_id = 100;
    order_evt.order_id.broker_id = 200;
    order_evt.status = event::OrderStatus::kFilled;
    order_evt.filled_quantity = 500;
    order_evt.filled_price = 34005000;  // 3400.50 * 10000
    order_evt.reject_reason = "";

    std::string json = bridge_->serialize_event(order_evt);

    EXPECT_TRUE(json.find("\"channel\":\"order\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"internal_id\":100") != std::string::npos);
    EXPECT_TRUE(json.find("\"status\":3") != std::string::npos);  // kFilled = 3
    EXPECT_TRUE(json.find("\"filled_price\":3400.50") != std::string::npos);
}

// ── Test: Serialize RiskAlertEvent ──
TEST_F(WsEventBridgeTest, SerializeRiskEvent) {
    bridge_ = std::make_unique<network::WsEventBridge>(*bus_, *ws_server_);

    event::RiskAlertEvent risk_evt;
    risk_evt.rule_id = 7;
    risk_evt.risk_level = event::RiskLevel::kRed;
    risk_evt.severity = event::RuleSeverity::kBlock;
    risk_evt.message = "Position limit exceeded";

    std::string json = bridge_->serialize_event(risk_evt);

    EXPECT_TRUE(json.find("\"channel\":\"risk\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"rule_id\":7") != std::string::npos);
    EXPECT_TRUE(json.find("\"risk_level\":2") != std::string::npos);  // kRed = 2
    EXPECT_TRUE(json.find("\"severity\":2") != std::string::npos);    // kBlock = 2
    EXPECT_TRUE(json.find("\"message\":\"Position limit exceeded\"") != std::string::npos);
}

// ── Test: Serialize TradeSignalEvent ──
TEST_F(WsEventBridgeTest, SerializeSignalEvent) {
    bridge_ = std::make_unique<network::WsEventBridge>(*bus_, *ws_server_);

    event::TradeSignalEvent sig_evt;
    sig_evt.strategy_id = "momentum_v1";
    sig_evt.symbol = "000001.SZ";
    sig_evt.side = event::OrderSide::kBuy;
    sig_evt.target_weight = 0.8;
    sig_evt.confidence = 0.92;

    std::string json = bridge_->serialize_event(sig_evt);

    EXPECT_TRUE(json.find("\"channel\":\"signal\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"strategy_id\":\"momentum_v1\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"symbol\":\"000001.SZ\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"side\":0") != std::string::npos);  // kBuy = 0
    EXPECT_TRUE(json.find("\"target_weight\":0.8000") != std::string::npos);
    EXPECT_TRUE(json.find("\"confidence\":0.9200") != std::string::npos);
}

// ── Test: Serialize MarketDataEvent ──
TEST_F(WsEventBridgeTest, SerializeMarketEvent) {
    bridge_ = std::make_unique<network::WsEventBridge>(*bus_, *ws_server_);

    event::MarketDataEvent mkt_evt;
    mkt_evt.symbol = "600519.SH";
    mkt_evt.last_price = 34005000;  // 3400.50 * 10000
    mkt_evt.volume = 100000;
    mkt_evt.bid_price1 = 34000000;  // 3400.00 * 10000
    mkt_evt.ask_price1 = 34010000;  // 3401.00 * 10000
    mkt_evt.bid_vol1 = 500;
    mkt_evt.ask_vol1 = 300;

    std::string json = bridge_->serialize_event(mkt_evt);

    EXPECT_TRUE(json.find("\"channel\":\"market\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"symbol\":\"600519.SH\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"last_price\":3400.50") != std::string::npos);
    EXPECT_TRUE(json.find("\"bid_price1\":3400.00") != std::string::npos);
    EXPECT_TRUE(json.find("\"ask_price1\":3401.00") != std::string::npos);
}

// ── Test: Bridge lifecycle (start/stop) ──
TEST_F(WsEventBridgeTest, LifecycleStartStop) {
    bridge_ = std::make_unique<network::WsEventBridge>(*bus_, *ws_server_);

    bool ok = bridge_->start();
    EXPECT_TRUE(ok);

    auto stats = bridge_->stats();
    EXPECT_EQ(stats.events_received, 0);
    EXPECT_EQ(stats.events_broadcast, 0);

    bridge_->stop();

    // Double stop should be safe
    bridge_->stop();
}

// ── Test: Bridge stats tracking ──
TEST_F(WsEventBridgeTest, StatsTracking) {
    bridge_ = std::make_unique<network::WsEventBridge>(*bus_, *ws_server_);
    bridge_->start();

    // Publish a kline event through EventBus
    auto kline_evt = std::make_unique<event::KlineEvent>();
    kline_evt->symbol = "600519.SH";
    kline_evt->kline_type = event::DataType::kKlineDay;
    kline_evt->kline.timestamp = 1700000000000;
    kline_evt->kline.open_price = 34005000;
    kline_evt->kline.high_price = 34200000;
    kline_evt->kline.low_price  = 33802500;
    kline_evt->kline.close_price = 34107500;
    kline_evt->kline.volume = 1500000;
    kline_evt->kline.amount = 5125000000;
    kline_evt->kline.vwap = 0;

    bus_->publish(std::move(kline_evt));

    auto stats = bridge_->stats();
    EXPECT_EQ(stats.events_received, 1);
    EXPECT_EQ(stats.events_broadcast, 1);
    EXPECT_EQ(stats.serialize_errors, 0);
}

// ── Test: Default channel mappings cover all event types ──
TEST_F(WsEventBridgeTest, DefaultChannelMappings) {
    auto mappings = network::WsEventBridge::default_channel_mappings();

    EXPECT_EQ(mappings.size(), 6u);

    // Verify each mapping has a non-empty channel name
    for (const auto& m : mappings) {
        EXPECT_TRUE(!m.channel.empty());
    }

    // Verify specific mappings
    bool has_kline = false, has_factor = false, has_order = false;
    bool has_risk = false, has_signal = false, has_market = false;
    for (const auto& m : mappings) {
        if (m.channel == "kline") has_kline = true;
        if (m.channel == "factor") has_factor = true;
        if (m.channel == "order") has_order = true;
        if (m.channel == "risk") has_risk = true;
        if (m.channel == "signal") has_signal = true;
        if (m.channel == "market") has_market = true;
    }
    EXPECT_TRUE(has_kline);
    EXPECT_TRUE(has_factor);
    EXPECT_TRUE(has_order);
    EXPECT_TRUE(has_risk);
    EXPECT_TRUE(has_signal);
    EXPECT_TRUE(has_market);
}

// ── Test: JSON envelope format ──
TEST_F(WsEventBridgeTest, JsonEnvelopeFormat) {
    bridge_ = std::make_unique<network::WsEventBridge>(*bus_, *ws_server_);

    event::KlineEvent kline_evt;
    kline_evt.symbol = "000001.SZ";
    kline_evt.kline_type = event::DataType::kKlineDay;
    kline_evt.kline.timestamp = 1700000000000;
    kline_evt.kline.open_price = 100000;
    kline_evt.kline.high_price = 110000;
    kline_evt.kline.low_price  = 90000;
    kline_evt.kline.close_price = 105000;
    kline_evt.kline.volume = 1000;
    kline_evt.kline.amount = 500000;
    kline_evt.kline.vwap = 0;

    std::string json = bridge_->serialize_event(kline_evt);

    // Verify JSON starts with { and ends with }
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');

    // Verify envelope contains all three required fields
    EXPECT_TRUE(json.find("\"channel\":") != std::string::npos);
    EXPECT_TRUE(json.find("\"data\":") != std::string::npos);
    EXPECT_TRUE(json.find("\"ts\":") != std::string::npos);
}