// risk_rule_test.cc — Tests for all risk rules including new additions (T4)
#include "cpp/quant/risk/risk_rule.h"
#include "cpp/quant/risk/risk_engine.h"
#include <gtest/gtest.h>

namespace quant::risk {
namespace {

// ── MaxPositionSizeRule tests ──

TEST(MaxPositionSizeRule, PassesWhenBelowLimit) {
    MaxPositionSizeRule rule(10000);
    RiskContext ctx;
    ctx.order_symbol = "600000.SS";
    ctx.order_quantity = 100;
    ctx.order_side = 1;
    ctx.symbol_quantities["600000.SS"] = 500;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(MaxPositionSizeRule, RejectsWhenExceedsLimit) {
    MaxPositionSizeRule rule(1000);
    RiskContext ctx;
    ctx.order_symbol = "600000.SS";
    ctx.order_quantity = 500;
    ctx.order_side = 1;
    ctx.symbol_quantities["600000.SS"] = 800;
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.rule_name, "MaxPositionSize");
}

TEST(MaxPositionSizeRule, SellReducesPosition) {
    MaxPositionSizeRule rule(1000);
    RiskContext ctx;
    ctx.order_symbol = "600000.SS";
    ctx.order_quantity = 200;
    ctx.order_side = -1;  // sell
    ctx.symbol_quantities["600000.SS"] = 1000;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(MaxPositionSizeRule, NewSymbolStartsAtZero) {
    MaxPositionSizeRule rule(2000);
    RiskContext ctx;
    ctx.order_symbol = "000001.SZ";
    ctx.order_quantity = 1500;
    ctx.order_side = 1;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(MaxPositionSizeRule, ValidateRejectsZero) {
    MaxPositionSizeRule rule(0);
    EXPECT_FALSE(rule.validate());
}

TEST(MaxPositionSizeRule, ValidateAcceptsPositive) {
    MaxPositionSizeRule rule(1000);
    EXPECT_TRUE(rule.validate());
}

// ── MaxOrderRateRule tests ──

TEST(MaxOrderRateRule, PassesWhenRateBelowLimit) {
    MaxOrderRateRule rule(60);
    RiskContext ctx;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(MaxOrderRateRule, ValidateRejectsZero) {
    MaxOrderRateRule rule(0);
    EXPECT_FALSE(rule.validate());
}

TEST(MaxOrderRateRule, ValidateAcceptsPositive) {
    MaxOrderRateRule rule(30);
    EXPECT_TRUE(rule.validate());
}

TEST(MaxOrderRateRule, RecordAndReset) {
    MaxOrderRateRule rule(2);
    rule.record_order();
    rule.record_order();
    rule.reset_rate();
    RiskContext ctx;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);  // Reset should clear history
}

TEST(MaxOrderRateRule, DisablePasses) {
    MaxOrderRateRule rule(0);
    rule.disable();
    RiskContext ctx;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

// ── TradingHoursRule tests ──

TEST(TradingHoursRule, ValidateAcceptsValidHours) {
    TradingHoursRule rule(9, 30, 15, 0);
    EXPECT_TRUE(rule.validate());
}

TEST(TradingHoursRule, ValidateRejectsInvalidHours) {
    TradingHoursRule bad_hour(25, 0, 15, 0);
    EXPECT_FALSE(bad_hour.validate());
}

TEST(TradingHoursRule, ValidateRejectsInvalidMinutes) {
    TradingHoursRule bad_min(9, 70, 15, 0);
    EXPECT_FALSE(bad_min.validate());
}

TEST(TradingHoursRule, Accessors) {
    TradingHoursRule rule(9, 30, 15, 0);
    EXPECT_EQ(rule.open_hour(), 9);
    EXPECT_EQ(rule.open_minute(), 30);
    EXPECT_EQ(rule.close_hour(), 15);
    EXPECT_EQ(rule.close_minute(), 0);
}

TEST(TradingHoursRule, DisablePasses) {
    TradingHoursRule rule(9, 30, 15, 0);
    rule.disable();
    RiskContext ctx;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

// ── PreTradeCheckRule tests ──

TEST(PreTradeCheckRule, PassesWhenAllChecksOk) {
    PreTradeCheckRule rule(0.10, 0.30, 1.0, 500000);
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 500000;
    ctx.max_drawdown = 0.05;
    ctx.order_symbol = "600000.SS";
    ctx.order_side = 1;
    ctx.order_quantity = 100;
    ctx.order_price = 50;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(PreTradeCheckRule, RejectsOnDrawdown) {
    PreTradeCheckRule rule(0.10, 0.30, 1.0, 500000);
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 500000;
    ctx.max_drawdown = 0.15;
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
}

TEST(PreTradeCheckRule, RejectsOnConcentration) {
    PreTradeCheckRule rule(0.10, 0.05, 1.0, 500000);  // 5% max concentration
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.max_drawdown = 0.01;
    ctx.order_symbol = "600000.SS";
    ctx.order_side = 1;
    ctx.order_quantity = 1000;
    ctx.order_price = 100;  // 100K = 10% of equity > 5%
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
}

TEST(PreTradeCheckRule, RejectsOnExposure) {
    PreTradeCheckRule rule(0.10, 0.30, 0.5, 500000);  // 50% max exposure
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 400000;
    ctx.max_drawdown = 0.01;
    ctx.order_symbol = "600000.SS";
    ctx.order_side = 1;
    ctx.order_quantity = 2000;
    ctx.order_price = 100;  // (400K + 200K) / 1M = 60% > 50%
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
}

TEST(PreTradeCheckRule, RejectsOnOrderValue) {
    PreTradeCheckRule rule(0.10, 0.30, 1.0, 50000);  // 50K max order value
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 500000;
    ctx.max_drawdown = 0.01;
    ctx.order_symbol = "600000.SS";
    ctx.order_side = 1;
    ctx.order_quantity = 1000;
    ctx.order_price = 100;  // 100K > 50K
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
}

TEST(PreTradeCheckRule, ValidateRejectsBadParams) {
    PreTradeCheckRule rule(0, 0.3, 1.0, 50000);
    EXPECT_FALSE(rule.validate());
}

TEST(PreTradeCheckRule, ValidateAcceptsGoodParams) {
    PreTradeCheckRule rule(0.10, 0.30, 1.0, 50000);
    EXPECT_TRUE(rule.validate());
}

// ── PortfolioRiskRule tests ──

TEST(PortfolioRiskRule, PassesWhenVarBelowLimit) {
    PortfolioRiskRule rule(0.10);
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 500000;
    ctx.market_volatility = 0.02;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(PortfolioRiskRule, RejectsWhenVarExceedsLimit) {
    PortfolioRiskRule rule(0.01);  // Very tight VaR limit
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 800000;
    ctx.market_volatility = 0.30;
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.rule_name, "PortfolioRisk");
}

TEST(PortfolioRiskRule, PassesWhenNoEquity) {
    PortfolioRiskRule rule(0.05);
    RiskContext ctx;
    ctx.total_equity = 0;
    ctx.total_positions = 500000;
    ctx.market_volatility = 0.30;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(PortfolioRiskRule, ValidateRejectsBadParams) {
    PortfolioRiskRule zero(0);
    EXPECT_FALSE(zero.validate());
    PortfolioRiskRule over1(1.5);
    EXPECT_FALSE(over1.validate());
}

TEST(PortfolioRiskRule, ValidateAcceptsGoodParams) {
    PortfolioRiskRule rule(0.05);
    EXPECT_TRUE(rule.validate());
}

// ── Integration: all new rules in engine ──

TEST(RiskEngine, NewRulesWithEngine) {
    RiskEngine engine;
    engine.register_rule(std::make_unique<MaxPositionSizeRule>(10000));
    engine.register_rule(std::make_unique<PortfolioRiskRule>(0.10));
    engine.register_rule(std::make_unique<PreTradeCheckRule>(0.10, 0.30, 1.5, 500000));

    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 500000;
    ctx.max_drawdown = 0.05;
    ctx.market_volatility = 0.02;
    ctx.order_symbol = "600000.SS";
    ctx.order_quantity = 100;
    ctx.order_side = 1;
    ctx.order_price = 50;

    auto result = engine.check(ctx);
    EXPECT_TRUE(result.approved);
    EXPECT_EQ(result.rule_results.size(), 3u);
}

TEST(RiskEngine, NewRulesRejectInEngine) {
    RiskEngine engine;
    engine.register_rule(std::make_unique<MaxPositionSizeRule>(100));
    engine.register_rule(std::make_unique<PortfolioRiskRule>(0.01));

    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 800000;
    ctx.market_volatility = 0.30;
    ctx.order_symbol = "600000.SS";
    ctx.symbol_quantities["600000.SS"] = 5000;
    ctx.order_quantity = 5000;
    ctx.order_side = 1;
    ctx.order_price = 100;

    auto result = engine.check(ctx);
    EXPECT_FALSE(result.approved);
}

// ── Existing rules regression tests ──

TEST(MaxDrawdownRule, DisabledPassesAlways) {
    MaxDrawdownRule rule(0.10);
    rule.disable();
    RiskContext ctx;
    ctx.max_drawdown = 0.50;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(ConcentrationRule, NewOrderCheck) {
    ConcentrationRule rule(0.30);
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.symbol_positions["600000.SS"] = 200000;
    ctx.order_symbol = "600000.SS";
    ctx.order_side = 1;
    ctx.order_quantity = 2000;
    ctx.order_price = 100;  // 200K order, existing 200K, total 400K = 40%
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
}

TEST(ExposureRule, SellSideDoesNotAddExposure) {
    ExposureRule rule(0.8);
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 900000;  // Already over 80%
    ctx.order_side = -1;  // selling, should not trigger exposure check
    ctx.order_price = 100;
    ctx.order_quantity = 100;
    auto result = rule.check(ctx);
    // Sell doesn't increase exposure, but current exposure is already over
    // 900K / 1M = 0.9 > 0.8, so this rejects based on current exposure
    EXPECT_FALSE(result.approved);
}

}  // namespace
}  // namespace quant::risk