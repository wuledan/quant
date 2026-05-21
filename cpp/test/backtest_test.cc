// backtest_test.cc — Unit tests for Portfolio, SimulatedBroker, BacktestRunner
#include <gtest/gtest.h>

#include <cmath>

#include "cpp/quant/backtest/simulated_broker.h"
#include "cpp/quant/backtest/backtest_runner.h"
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/execution/order.h"
#include "cpp/quant/portfolio/portfolio.h"

namespace bt = quant::backtest;
namespace pf = quant::portfolio;
namespace ex = quant::execution;
namespace ev = quant::event;

// ── Portfolio tests ──

TEST(PortfolioTest, InitialState) {
    pf::Portfolio p(100000.0);
    EXPECT_DOUBLE_EQ(p.cash(), 100000.0);
    EXPECT_DOUBLE_EQ(p.total_value(), 100000.0);
    EXPECT_DOUBLE_EQ(p.position_value(), 0.0);
    EXPECT_TRUE(p.nav_history().empty());
}

TEST(PortfolioTest, BuyUpdatesPosition) {
    pf::Portfolio p(100000.0);
    p.on_fill("600519.SH", ex::OrderSide::kBuy, 100.0, 1800.0, 5.0);
    EXPECT_NEAR(p.cash(), 100000.0 - 180000.0 - 5.0, 1e-6);

    auto* pos = p.get_position("600519.SH");
    ASSERT_NE(pos, nullptr);
    EXPECT_DOUBLE_EQ(pos->quantity, 100.0);
    EXPECT_DOUBLE_EQ(pos->avg_cost, 1800.0);
}

TEST(PortfolioTest, SellReducesPosition) {
    pf::Portfolio p(100000.0);
    p.on_fill("600519.SH", ex::OrderSide::kBuy, 100.0, 1800.0, 5.0);
    p.on_fill("600519.SH", ex::OrderSide::kSell, 50.0, 1900.0, 5.0);

    auto* pos = p.get_position("600519.SH");
    ASSERT_NE(pos, nullptr);
    EXPECT_DOUBLE_EQ(pos->quantity, 50.0);
    EXPECT_DOUBLE_EQ(pos->avg_cost, 1800.0);
}

TEST(PortfolioTest, NavTracking) {
    pf::Portfolio p(100000.0);
    p.on_fill("600519.SH", ex::OrderSide::kBuy, 10.0, 1000.0, 5.0);
    p.update_market_value("600519.SH", 1050.0);
    p.record_nav(1000);

    ASSERT_EQ(p.nav_history().size(), 1u);
    double expected_nav = (100000.0 - 10000.0 - 5.0) + 10.0 * 1050.0;
    EXPECT_NEAR(p.nav_history()[0].second, expected_nav, 1e-6);
}

TEST(PortfolioTest, UnrealizedPnL) {
    pf::Portfolio p(100000.0);
    p.on_fill("600519.SH", ex::OrderSide::kBuy, 100.0, 1800.0, 5.0);
    p.update_market_value("600519.SH", 1900.0);
    EXPECT_NEAR(p.unrealized_pnl(), 100.0 * 100.0, 1e-6);
}

TEST(PortfolioTest, MultipleBuysAvgCost) {
    pf::Portfolio p(100000.0);
    p.on_fill("600519.SH", ex::OrderSide::kBuy, 100.0, 1800.0, 0.0);
    p.on_fill("600519.SH", ex::OrderSide::kBuy, 100.0, 2000.0, 0.0);

    auto* pos = p.get_position("600519.SH");
    ASSERT_NE(pos, nullptr);
    EXPECT_DOUBLE_EQ(pos->quantity, 200.0);
    EXPECT_DOUBLE_EQ(pos->avg_cost, 1900.0);
}

// ── SimulatedBroker tests ──

TEST(SimulatedBrokerTest, BasicExecution) {
    bt::SimulatedBroker broker;
    auto fill = broker.execute("600519.SH", ex::OrderSide::kBuy, 100.0, 1800.0, 1000);
    EXPECT_DOUBLE_EQ(fill.quantity, 100.0);
    EXPECT_GT(fill.price, 1800.0);
    EXPECT_GT(fill.commission, 0.0);
    EXPECT_EQ(fill.timestamp, 1000);
}

TEST(SimulatedBrokerTest, CommissionCalculation) {
    bt::SimulatedBrokerConfig cfg;
    cfg.commission_rate = 0.001;
    cfg.min_commission = 10.0;
    cfg.enable_slippage = false;
    bt::SimulatedBroker broker(cfg);

    auto fill = broker.execute("600519.SH", ex::OrderSide::kBuy, 100.0, 1800.0, 0);
    double expected_commission = 1800.0 * 100.0 * 0.001;
    EXPECT_NEAR(fill.commission, expected_commission, 1e-6);
}

TEST(SimulatedBrokerTest, MinCommission) {
    bt::SimulatedBrokerConfig cfg;
    cfg.commission_rate = 0.0001;
    cfg.min_commission = 5.0;
    cfg.enable_slippage = false;
    bt::SimulatedBroker broker(cfg);

    auto fill = broker.execute("600519.SH", ex::OrderSide::kBuy, 1.0, 10.0, 0);
    EXPECT_DOUBLE_EQ(fill.commission, 5.0);
}

TEST(SimulatedBrokerTest, SlippageDirection) {
    bt::SimulatedBrokerConfig cfg;
    cfg.slippage_bps = 10.0;
    cfg.enable_slippage = true;
    bt::SimulatedBroker broker(cfg);

    auto buy_fill = broker.execute("600519.SH", ex::OrderSide::kBuy, 100.0, 1800.0, 0);
    auto sell_fill = broker.execute("600519.SH", ex::OrderSide::kSell, 100.0, 1800.0, 0);

    EXPECT_GT(buy_fill.price, 1800.0);
    EXPECT_LT(sell_fill.price, 1800.0);
}

TEST(SimulatedBrokerTest, NoSlippage) {
    bt::SimulatedBrokerConfig cfg;
    cfg.enable_slippage = false;
    bt::SimulatedBroker broker(cfg);

    auto fill = broker.execute("600519.SH", ex::OrderSide::kBuy, 100.0, 1800.0, 0);
    EXPECT_DOUBLE_EQ(fill.price, 1800.0);
}

// ── BacktestResult/Params tests ──

TEST(BacktestResultTest, EmptyResult) {
    bt::BacktestResult result;
    EXPECT_DOUBLE_EQ(result.total_return, 0.0);
    EXPECT_DOUBLE_EQ(result.max_drawdown, 0.0);
    EXPECT_DOUBLE_EQ(result.sharpe_ratio, 0.0);
    EXPECT_EQ(result.total_trades, 0);
}

TEST(BacktestParamsTest, DefaultValues) {
    bt::BacktestParams params;
    EXPECT_DOUBLE_EQ(params.initial_cash, 1000000.0);
    EXPECT_EQ(params.kline_type, ev::DataType::kKlineDay);
}