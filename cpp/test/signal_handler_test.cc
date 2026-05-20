// signal_handler_test.cc — Unit tests for signal handlers
#include <gtest/gtest.h>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/execution/order_manager.h"
#include "cpp/quant/strategy/signal_handler.h"

using namespace quant::strategy;
using namespace quant::event;
using namespace quant::execution;

class SignalHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        bus_ = std::make_unique<EventBus>(EventBus::Options{});
        order_mgr_ = std::make_unique<OrderManager>();
    }

    void TearDown() override {
        order_mgr_.reset();
        bus_.reset();
    }

    std::unique_ptr<EventBus> bus_;
    std::unique_ptr<OrderManager> order_mgr_;
};

// ── Test: OrderSignalHandler buy on positive signal ──
TEST_F(SignalHandlerTest, OrderHandlerBuyOnPositiveSignal) {
    OrderSignalHandler::Params params;
    params.buy_weight = 0.95;
    params.sell_weight = 1.0;

    OrderSignalHandler handler(params, *order_mgr_, *bus_);

    SignalContext ctx;
    ctx.symbol = "600519.SH";
    ctx.price = 1800.0;
    ctx.cash = 100000.0;
    ctx.position = 0.0;
    ctx.signal_value = 1.0;

    handler.handle(1.0, ctx);
    EXPECT_EQ(handler.orders_generated(), 1);
    EXPECT_EQ(order_mgr_->total_order_count(), 1);
}

// ── Test: OrderSignalHandler sell on negative signal ──
TEST_F(SignalHandlerTest, OrderHandlerSellOnNegativeSignal) {
    OrderSignalHandler::Params params;
    params.buy_weight = 0.95;
    params.sell_weight = 1.0;

    OrderSignalHandler handler(params, *order_mgr_, *bus_);

    SignalContext ctx;
    ctx.symbol = "600519.SH";
    ctx.price = 1800.0;
    ctx.cash = 0.0;
    ctx.position = 100.0;
    ctx.signal_value = -1.0;

    handler.handle(-1.0, ctx);
    EXPECT_EQ(handler.orders_generated(), 1);
    EXPECT_EQ(order_mgr_->total_order_count(), 1);
}

// ── Test: OrderSignalHandler ignores zero signal ──
TEST_F(SignalHandlerTest, OrderHandlerIgnoresZeroSignal) {
    OrderSignalHandler::Params params;
    OrderSignalHandler handler(params, *order_mgr_, *bus_);

    SignalContext ctx;
    ctx.symbol = "600519.SH";
    ctx.price = 1800.0;
    ctx.cash = 100000.0;

    handler.handle(0.0, ctx);
    EXPECT_EQ(handler.orders_generated(), 0);
    EXPECT_EQ(order_mgr_->total_order_count(), 0);
}

// ── Test: OrderSignalHandler respects min_signal threshold ──
TEST_F(SignalHandlerTest, OrderHandlerMinSignalThreshold) {
    OrderSignalHandler::Params params;
    params.min_signal = 0.5;

    OrderSignalHandler handler(params, *order_mgr_, *bus_);

    SignalContext ctx;
    ctx.symbol = "600519.SH";
    ctx.price = 1800.0;
    ctx.cash = 100000.0;

    // Signal below threshold → no order
    handler.handle(0.3, ctx);
    EXPECT_EQ(handler.orders_generated(), 0);

    // Signal above threshold → order
    handler.handle(0.6, ctx);
    EXPECT_EQ(handler.orders_generated(), 1);
}

// ── Test: OrderSignalHandler no order when zero cash ──
TEST_F(SignalHandlerTest, OrderHandlerNoBuyWithZeroCash) {
    OrderSignalHandler::Params params;
    OrderSignalHandler handler(params, *order_mgr_, *bus_);

    SignalContext ctx;
    ctx.symbol = "600519.SH";
    ctx.price = 1800.0;
    ctx.cash = 0.0;
    ctx.position = 0.0;

    handler.handle(1.0, ctx);
    EXPECT_EQ(handler.orders_generated(), 0);
}

// ── Test: OrderSignalHandler no sell with zero position ──
TEST_F(SignalHandlerTest, OrderHandlerNoSellWithZeroPosition) {
    OrderSignalHandler::Params params;
    OrderSignalHandler handler(params, *order_mgr_, *bus_);

    SignalContext ctx;
    ctx.symbol = "600519.SH";
    ctx.price = 1800.0;
    ctx.cash = 0.0;
    ctx.position = 0.0;

    handler.handle(-1.0, ctx);
    EXPECT_EQ(handler.orders_generated(), 0);
}

// ── Test: AlertSignalHandler publishes event ──
TEST_F(SignalHandlerTest, AlertHandlerPublishesEvent) {
    AlertSignalHandler handler(*bus_);

    SignalContext ctx;
    ctx.symbol = "000001.SH";
    ctx.price = 3400.0;

    handler.handle(1.0, ctx);
    EXPECT_EQ(handler.alerts_sent(), 1);
}

// ── Test: SignalHandlerFactory creates order handler ──
TEST_F(SignalHandlerTest, FactoryCreatesOrderHandler) {
    auto handler = SignalHandlerFactory::create(
        "order", {{"buy_weight", 0.9}}, *order_mgr_, *bus_);
    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->handler_type(), "order");
}

// ── Test: SignalHandlerFactory creates alert handler ──
TEST_F(SignalHandlerTest, FactoryCreatesAlertHandler) {
    auto handler = SignalHandlerFactory::create(
        "alert", {}, *order_mgr_, *bus_);
    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->handler_type(), "alert");
}

// ── Test: SignalHandlerFactory returns null for unknown type ──
TEST_F(SignalHandlerTest, FactoryUnknownType) {
    auto handler = SignalHandlerFactory::create(
        "unknown", {}, *order_mgr_, *bus_);
    EXPECT_EQ(handler, nullptr);
}
