// risk_test.cc — Tests for risk engine components
#include "cpp/quant/risk/risk_rule.h"
#include "cpp/quant/risk/risk_engine.h"
#include "cpp/quant/risk/risk_alert_publisher.h"
#include <gtest/gtest.h>

namespace quant::risk {
namespace {

// ── RiskRule tests ──

TEST(MaxDrawdownRule, PassesWhenBelowLimit) {
    MaxDrawdownRule rule(0.10);
    RiskContext ctx;
    ctx.max_drawdown = 0.05;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(MaxDrawdownRule, RejectsWhenAboveLimit) {
    MaxDrawdownRule rule(0.10);
    RiskContext ctx;
    ctx.max_drawdown = 0.15;
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.rule_name, "MaxDrawdown");
}

TEST(ConcentrationRule, PassesWhenDiversified) {
    ConcentrationRule rule(0.20);
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.symbol_positions["600000.SS"] = 150000;  // 15%
    ctx.symbol_positions["000001.SZ"] = 100000;  // 10%
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(ConcentrationRule, RejectsWhenConcentrated) {
    ConcentrationRule rule(0.20);
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.symbol_positions["600000.SS"] = 500000;  // 50%
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
}

TEST(ExposureRule, PassesWhenWithinLimit) {
    ExposureRule rule(1.5);
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 800000;
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(ExposureRule, RejectsWhenExceeded) {
    ExposureRule rule(0.8);
    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 900000;
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
}

TEST(LimitRule, RejectsLargeOrder) {
    LimitRule rule(500000, 5000000);
    RiskContext ctx;
    ctx.order_side = 1;  // buy
    ctx.order_quantity = 1000;
    ctx.order_price = 1000;  // 1M order value > 500K limit
    auto result = rule.check(ctx);
    EXPECT_FALSE(result.approved);
}

TEST(LimitRule, PassesSmallOrder) {
    LimitRule rule(500000, 5000000);
    RiskContext ctx;
    ctx.order_side = 1;
    ctx.order_quantity = 100;
    ctx.order_price = 100;   // 10K < 500K
    auto result = rule.check(ctx);
    EXPECT_TRUE(result.approved);
}

// ── RiskEngine tests ──

TEST(RiskEngine, EmptyEnginePasses) {
    RiskEngine engine;
    RiskContext ctx;
    ctx.total_equity = 1000000;

    auto result = engine.check(ctx);
    EXPECT_TRUE(result.approved);
    EXPECT_EQ(result.rule_results.size(), 0u);
}

TEST(RiskEngine, SingleRule) {
    RiskEngine engine;
    engine.register_rule(std::make_unique<MaxDrawdownRule>(0.10));

    RiskContext ctx;
    ctx.max_drawdown = 0.15;
    auto result = engine.check(ctx);
    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.rule_results.size(), 1u);
}

TEST(RiskEngine, MultipleRules) {
    RiskEngine engine;
    engine.register_rule(std::make_unique<MaxDrawdownRule>(0.10));
    engine.register_rule(std::make_unique<ExposureRule>(1.5));

    RiskContext ctx;
    ctx.total_equity = 1000000;
    ctx.total_positions = 500000;
    ctx.max_drawdown = 0.15;  // fails this one

    auto result = engine.check(ctx);
    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.rule_results.size(), 2u);
}

TEST(RiskEngine, DisabledEngine) {
    RiskEngine engine;
    engine.register_rule(std::make_unique<MaxDrawdownRule>(0.10));
    engine.disable();

    RiskContext ctx;
    ctx.max_drawdown = 0.50;
    auto result = engine.check(ctx);
    EXPECT_TRUE(result.approved);  // Engine disabled, all pass
}

TEST(RiskEngine, DisableRule) {
    RiskEngine engine;
    auto rule = std::make_unique<MaxDrawdownRule>(0.10);
    auto* rule_ptr = rule.get();
    engine.register_rule(std::move(rule));
    rule_ptr->disable();

    RiskContext ctx;
    ctx.max_drawdown = 0.50;
    auto result = engine.check(ctx);
    EXPECT_TRUE(result.approved);
}

TEST(RiskEngine, TrackStats) {
    RiskEngine engine;
    engine.register_rule(std::make_unique<MaxDrawdownRule>(0.10));

    RiskContext ctx;
    ctx.max_drawdown = 0.05;  // pass
    engine.check(ctx);
    ctx.max_drawdown = 0.15;  // fail
    engine.check(ctx);

    auto s = engine.stats();
    EXPECT_EQ(s.total_checks, 2u);
    EXPECT_EQ(s.total_approvals, 1u);
    EXPECT_EQ(s.total_rejections, 1u);
}

TEST(RiskEngine, FindAndUnregisterRule) {
    RiskEngine engine;
    engine.register_rule(std::make_unique<MaxDrawdownRule>(0.10));

    auto* rule = engine.find_rule(1);
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->name(), "MaxDrawdown");

    engine.unregister_rule(1);
    EXPECT_EQ(engine.find_rule(1), nullptr);
}

TEST(RiskEngine, AllRules) {
    RiskEngine engine;
    engine.register_rule(std::make_unique<MaxDrawdownRule>(0.10));
    engine.register_rule(std::make_unique<ExposureRule>(1.5));

    auto rules = engine.all_rules();
    EXPECT_EQ(rules.size(), 2u);
}

// ── RiskAlertPublisher tests ──

TEST(RiskAlertPublisher, PublishAlert) {
    auto bus_opts = quant::event::EventBus::default_options();
    quant::event::EventBus bus(bus_opts);
    RiskAlertPublisher publisher(bus);

    RiskAlert alert;
    alert.rule_id = 1;
    alert.rule_name = "TestRule";
    alert.severity = AlertSeverity::kWarning;
    alert.message = "test alert";
    alert.timestamp_ns = 1000000;

    // Should not throw
    EXPECT_NO_THROW(publisher.publish(alert));
}

}  // namespace
}  // namespace quant::risk