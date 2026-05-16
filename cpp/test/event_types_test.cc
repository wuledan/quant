// event_types_test.cc — Tests for event type definitions
#include "cpp/quant/event/events/market_data_event.h"
#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/event/events/trade_signal_event.h"
#include "cpp/quant/event/events/order_report_event.h"
#include "cpp/quant/event/events/risk_alert_event.h"
#include "cpp/quant/event/events/factor_update_event.h"
#include <gtest/gtest.h>

namespace quant::event {
namespace {

TEST(EventTypesTest, MarketDataEventFields) {
    MarketDataEvent e;
    e.symbol = "000300.SH";
    e.last_price = 43250000;  // 4325.0000
    e.volume = 1000000;
    e.bid_price1 = 43240000;
    e.ask_price1 = 43260000;
    e.bid_vol1 = 5000;
    e.ask_vol1 = 3000;

    EXPECT_EQ(e.event_type_id(), 1);
    EXPECT_EQ(e.event_name(), "MarketDataEvent");
    EXPECT_EQ(e.symbol, "000300.SH");
    EXPECT_EQ(e.last_price, 43250000);
    EXPECT_EQ(e.volume, 1000000);
    EXPECT_GE(e.sequence(), 0);
}

TEST(EventTypesTest, KlineEventFields) {
    KlineEvent e;
    e.symbol = "000300.SH";
    e.kline_type = DataType::kKline1Min;
    e.kline.timestamp = 1700000000000000;
    e.kline.open_price = 43000000;
    e.kline.high_price = 43500000;
    e.kline.low_price = 42800000;
    e.kline.close_price = 43300000;
    e.kline.volume = 5000000;
    e.kline.amount = 215000000000LL;
    e.kline.vwap = 43200000;

    EXPECT_EQ(e.event_type_id(), 2);
    EXPECT_EQ(e.event_name(), "KlineEvent");
    EXPECT_EQ(e.kline_type, DataType::kKline1Min);
    EXPECT_EQ(sizeof(KlineRow), 48);
}

TEST(EventTypesTest, TradeSignalEventFields) {
    TradeSignalEvent e;
    e.strategy_id = "momentum_001";
    e.symbol = "000300.SH";
    e.side = OrderSide::kBuy;
    e.target_weight = 0.8;
    e.confidence = 0.75;

    EXPECT_EQ(e.event_type_id(), 3);
    EXPECT_EQ(e.event_name(), "TradeSignalEvent");
    EXPECT_EQ(e.side, OrderSide::kBuy);
    EXPECT_DOUBLE_EQ(e.target_weight, 0.8);
    EXPECT_DOUBLE_EQ(e.confidence, 0.75);
}

TEST(EventTypesTest, OrderReportEventFields) {
    OrderReportEvent e;
    e.order_id = {1001, 50001};
    e.status = OrderStatus::kFilled;
    e.filled_quantity = 1000;
    e.filled_price = 43250000;
    e.reject_reason = "";

    EXPECT_EQ(e.event_type_id(), 4);
    EXPECT_EQ(e.event_name(), "OrderReportEvent");
    EXPECT_EQ(e.order_id.internal_id, 1001);
    EXPECT_EQ(e.order_id.broker_id, 50001);
    EXPECT_EQ(e.status, OrderStatus::kFilled);
    EXPECT_TRUE((e.order_id == OrderId{1001, 50001}));
}

TEST(EventTypesTest, RiskAlertEventFields) {
    RiskAlertEvent e;
    e.rule_id = 42;
    e.risk_level = RiskLevel::kRed;
    e.severity = RuleSeverity::kBlock;
    e.message = "Position limit exceeded";

    EXPECT_EQ(e.event_type_id(), 5);
    EXPECT_EQ(e.event_name(), "RiskAlertEvent");
    EXPECT_EQ(e.rule_id, 42);
    EXPECT_EQ(e.risk_level, RiskLevel::kRed);
    EXPECT_EQ(e.severity, RuleSeverity::kBlock);
}

TEST(EventTypesTest, FactorUpdateEventFields) {
    FactorUpdateEvent e;
    e.factor_id = 7;
    e.trading_day = 20260515;

    EXPECT_EQ(e.event_type_id(), 6);
    EXPECT_EQ(e.event_name(), "FactorUpdateEvent");
    EXPECT_EQ(e.factor_id, 7);
    EXPECT_EQ(e.trading_day, 20260515);
}

TEST(EventTypesTest, EventTimestampAndSequence) {
    MarketDataEvent e1;
    e1.set_timestamp_us(1700000000000000);
    MarketDataEvent e2;
    e2.set_timestamp_us(1700000000000001);

    EXPECT_EQ(e1.timestamp_us(), 1700000000000000);
    EXPECT_EQ(e2.timestamp_us(), 1700000000000001);
    EXPECT_NE(e1.sequence(), e2.sequence());
    EXPECT_GT(e2.sequence(), e1.sequence());
}

TEST(EventTypesTest, EventTypeIdUniqueness) {
    EXPECT_NE(MarketDataEvent::kEventTypeId, KlineEvent::kEventTypeId);
    EXPECT_NE(KlineEvent::kEventTypeId, TradeSignalEvent::kEventTypeId);
    EXPECT_NE(TradeSignalEvent::kEventTypeId, OrderReportEvent::kEventTypeId);
    EXPECT_NE(OrderReportEvent::kEventTypeId, RiskAlertEvent::kEventTypeId);
    EXPECT_NE(RiskAlertEvent::kEventTypeId, FactorUpdateEvent::kEventTypeId);
}

}  // namespace
}  // namespace quant::event
