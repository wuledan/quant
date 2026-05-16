// risk_rule.h — Risk rule interface and built-in rules
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace quant::risk {

// ── Rule identifiers ──
using RuleId = uint32_t;

// ── Risk check result ──
struct RiskCheckResult {
    bool        approved = true;
    RuleId      rule_id = 0;
    std::string rule_name;
    std::string message;
    double      exposure = 0.0;   // current exposure value
    double      limit = 0.0;      // limit threshold

    static RiskCheckResult pass() { return {}; }
    static RiskCheckResult reject(RuleId id, std::string name,
                                   std::string msg, double exposure = 0, double limit = 0) {
        return {false, id, std::move(name), std::move(msg), exposure, limit};
    }
};

// ── Risk context: provides portfolio/market state to rules ──
struct RiskContext {
    double      total_equity      = 0.0;  // 总权益
    double      available_cash     = 0.0;  // 可用现金
    double      total_positions    = 0.0;  // 总持仓市值
    double      unrealized_pnl     = 0.0;  // 未实现盈亏
    double      realized_pnl       = 0.0;  // 已实现盈亏
    double      daily_pnl          = 0.0;  // 当日盈亏
    double      max_drawdown       = 0.0;  // 最大回撤

    // Per-symbol data
    std::unordered_map<std::string, double> symbol_positions;   // symbol -> position value
    std::unordered_map<std::string, double> symbol_quantities;   // symbol -> held quantity
    std::unordered_map<std::string, double> symbol_prices;       // symbol -> current price

    // Pending order info
    std::string order_symbol;
    int64_t     order_quantity    = 0;
    double      order_price       = 0.0;
    int         order_side        = 0;  // 1=buy, -1=sell

    // Market data
    double      market_volatility = 0.0;  // market VIX equivalent
};

// ── Risk rule interface ──
class IRiskRule {
public:
    virtual ~IRiskRule() = default;

    virtual RuleId id() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;

    // ── Check rule against context ──
    virtual RiskCheckResult check(const RiskContext& ctx) const = 0;

    // ── Validate if rule is properly configured ──
    virtual bool validate() const noexcept = 0;

    // ── Enable/disable ──
    virtual void enable() noexcept { enabled_ = true; }
    virtual void disable() noexcept { enabled_ = false; }
    virtual bool is_enabled() const noexcept { return enabled_; }

protected:
    bool enabled_ = true;
};

// ── Max drawdown rule ──
class MaxDrawdownRule : public IRiskRule {
public:
    explicit MaxDrawdownRule(double max_drawdown_pct)
        : max_drawdown_pct_(max_drawdown_pct) {}

    RuleId id() const noexcept override { return 1; }
    std::string_view name() const noexcept override { return "MaxDrawdown"; }

    RiskCheckResult check(const RiskContext& ctx) const override {
        if (!enabled_) return RiskCheckResult::pass();
        if (ctx.max_drawdown <= max_drawdown_pct_) {
            return RiskCheckResult::pass();
        }
        return RiskCheckResult::reject(id(), std::string(name()),
            "Max drawdown exceeded", ctx.max_drawdown, max_drawdown_pct_);
    }

    bool validate() const noexcept override { return max_drawdown_pct_ > 0 && max_drawdown_pct_ <= 1.0; }
    double max_drawdown_pct() const noexcept { return max_drawdown_pct_; }

private:
    double max_drawdown_pct_;
};

// ── Concentration rule: single position shouldn't exceed threshold ──
class ConcentrationRule : public IRiskRule {
public:
    explicit ConcentrationRule(double max_concentration_pct)
        : max_concentration_pct_(max_concentration_pct) {}

    RuleId id() const noexcept override { return 2; }
    std::string_view name() const noexcept override { return "Concentration"; }

    RiskCheckResult check(const RiskContext& ctx) const override {
        if (!enabled_) return RiskCheckResult::pass();
        if (ctx.total_equity <= 0) return RiskCheckResult::pass();

        // Check all existing positions
        for (const auto& [symbol, value] : ctx.symbol_positions) {
            double conc = value / ctx.total_equity;
            if (conc > max_concentration_pct_) {
                return RiskCheckResult::reject(id(), std::string(name()),
                    "Concentration too high for " + symbol, conc, max_concentration_pct_);
            }
        }

        // Check new order effect
        if (!ctx.order_symbol.empty() && ctx.order_side == 1 && ctx.order_price > 0) {
            double order_value = ctx.order_quantity * ctx.order_price;
            double new_conc = order_value / ctx.total_equity;
            double existing = 0;
            auto it = ctx.symbol_positions.find(ctx.order_symbol);
            if (it != ctx.symbol_positions.end()) existing = it->second / ctx.total_equity;
            if (existing + new_conc > max_concentration_pct_) {
                return RiskCheckResult::reject(id(), std::string(name()),
                    "Order would exceed concentration for " + ctx.order_symbol,
                    existing + new_conc, max_concentration_pct_);
            }
        }
        return RiskCheckResult::pass();
    }

    bool validate() const noexcept override { return max_concentration_pct_ > 0 && max_concentration_pct_ <= 1.0; }
    double max_concentration_pct() const noexcept { return max_concentration_pct_; }

private:
    double max_concentration_pct_;
};

// ── Exposure rule: total position exposure shouldn't exceed limit ──
class ExposureRule : public IRiskRule {
public:
    explicit ExposureRule(double max_exposure_ratio)
        : max_exposure_ratio_(max_exposure_ratio) {}

    RuleId id() const noexcept override { return 3; }
    std::string_view name() const noexcept override { return "Exposure"; }

    RiskCheckResult check(const RiskContext& ctx) const override {
        if (!enabled_) return RiskCheckResult::pass();
        if (ctx.total_equity <= 0) return RiskCheckResult::pass();

        double current_exposure = ctx.total_positions / ctx.total_equity;
        if (current_exposure > max_exposure_ratio_) {
            return RiskCheckResult::reject(id(), std::string(name()),
                "Exposure too high", current_exposure, max_exposure_ratio_);
        }

        // Check new order effect
        if (ctx.order_side == 1 && ctx.order_price > 0) {
            double order_value = ctx.order_quantity * ctx.order_price;
            double new_exposure = (ctx.total_positions + order_value) / ctx.total_equity;
            if (new_exposure > max_exposure_ratio_) {
                return RiskCheckResult::reject(id(), std::string(name()),
                    "Order would exceed exposure limit", new_exposure, max_exposure_ratio_);
            }
        }
        return RiskCheckResult::pass();
    }

    bool validate() const noexcept override { return max_exposure_ratio_ > 0; }
    double max_exposure_ratio() const noexcept { return max_exposure_ratio_; }

private:
    double max_exposure_ratio_;
};

// ── Limit rule: absolute position/value limits ──
class LimitRule : public IRiskRule {
public:
    LimitRule(double max_order_value, double max_total_value)
        : max_order_value_(max_order_value), max_total_value_(max_total_value) {}

    RuleId id() const noexcept override { return 4; }
    std::string_view name() const noexcept override { return "Limit"; }

    RiskCheckResult check(const RiskContext& ctx) const override {
        if (!enabled_) return RiskCheckResult::pass();

        // Check individual order value
        if (ctx.order_side == 1 && ctx.order_price > 0) {
            double order_value = ctx.order_quantity * ctx.order_price;
            if (order_value > max_order_value_) {
                return RiskCheckResult::reject(id(), std::string(name()),
                    "Order value exceeds single-order limit", order_value, max_order_value_);
            }
        }

        // Check total position value
        if (ctx.total_positions > max_total_value_) {
            return RiskCheckResult::reject(id(), std::string(name()),
                "Total position value exceeds limit", ctx.total_positions, max_total_value_);
        }
        return RiskCheckResult::pass();
    }

    bool validate() const noexcept override { return max_order_value_ > 0 && max_total_value_ > 0; }
    double max_order_value() const noexcept { return max_order_value_; }
    double max_total_value() const noexcept { return max_total_value_; }

private:
    double max_order_value_;
    double max_total_value_;
};

}  // namespace quant::risk