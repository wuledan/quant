// risk_rule.cc — Additional risk rule implementations (T4)
#include "cpp/quant/risk/risk_rule.h"

#include <chrono>
#include <cmath>

namespace quant::risk {

// ── MaxPositionSizeRule ──
RiskCheckResult MaxPositionSizeRule::check(const RiskContext& ctx) const {
    if (!enabled_) return RiskCheckResult::pass();

    double abs_qty = std::abs(static_cast<double>(ctx.order_quantity));
    auto it = ctx.symbol_quantities.find(ctx.order_symbol);
    double current_qty = 0;
    if (it != ctx.symbol_quantities.end()) {
        current_qty = std::abs(it->second);
    }

    double new_qty = (ctx.order_side == 1) ? current_qty + abs_qty : current_qty - abs_qty;
    if (new_qty > max_position_size_) {
        return RiskCheckResult::reject(id(), std::string(name()),
            "Order would exceed max position size for " + ctx.order_symbol,
            new_qty, max_position_size_);
    }
    return RiskCheckResult::pass();
}

bool MaxPositionSizeRule::validate() const noexcept {
    return max_position_size_ > 0;
}

// ── MaxOrderRateRule ──
RiskCheckResult MaxOrderRateRule::check(const RiskContext& ctx) const {
    if (!enabled_) return RiskCheckResult::pass();

    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t window_ns = 60'000'000'000LL;  // 1 minute in nanoseconds

    // Prune timestamps older than the window
    auto cutoff = now_ns - window_ns;
    order_timestamps_ns_.erase(
        std::remove_if(order_timestamps_ns_.begin(), order_timestamps_ns_.end(),
            [cutoff](int64_t ts) { return ts < cutoff; }),
        order_timestamps_ns_.end());

    double rate = static_cast<double>(order_timestamps_ns_.size());
    if (rate > max_orders_per_minute_) {
        return RiskCheckResult::reject(id(), std::string(name()),
            "Order rate exceeds limit", rate, max_orders_per_minute_);
    }
    return RiskCheckResult::pass();
}

bool MaxOrderRateRule::validate() const noexcept {
    return max_orders_per_minute_ > 0;
}

void MaxOrderRateRule::record_order() {
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    order_timestamps_ns_.push_back(now_ns);
}

void MaxOrderRateRule::reset_rate() {
    order_timestamps_ns_.clear();
}

// ── TradingHoursRule ──
RiskCheckResult TradingHoursRule::check(const RiskContext& ctx) const {
    if (!enabled_) return RiskCheckResult::pass();

    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (!is_within_hours(now_ns)) {
        return RiskCheckResult::reject(id(), std::string(name()),
            "Order outside trading hours", 0, 0);
    }
    return RiskCheckResult::pass();
}

bool TradingHoursRule::validate() const noexcept {
    return open_hour_ >= 0 && open_hour_ <= 23
        && close_hour_ >= 0 && close_hour_ <= 23
        && open_minute_ >= 0 && open_minute_ < 60
        && close_minute_ >= 0 && close_minute_ < 60;
}

bool TradingHoursRule::is_within_hours(int64_t timestamp_ns) const {
    auto secs = std::chrono::seconds(timestamp_ns / 1'000'000'000);
    auto day_time = std::chrono::floor<std::chrono::days>(secs);
    auto time_of_day = secs - day_time;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(time_of_day).count();
    auto mins = std::chrono::duration_cast<std::chrono::minutes>(time_of_day).count() % 60;

    int current = static_cast<int>(hours) * 60 + static_cast<int>(mins);
    int open = open_hour_ * 60 + open_minute_;
    int close = close_hour_ * 60 + close_minute_;
    return current >= open && current <= close;
}

// ── PreTradeCheckRule ──
RiskCheckResult PreTradeCheckRule::check(const RiskContext& ctx) const {
    if (!enabled_) return RiskCheckResult::pass();

    // 1. Drawdown check
    if (ctx.max_drawdown > max_drawdown_pct_) {
        return RiskCheckResult::reject(id(), std::string(name()),
            "Pre-trade: max drawdown exceeded", ctx.max_drawdown, max_drawdown_pct_);
    }

    // 2. Concentration check for the order's symbol
    if (ctx.total_equity > 0 && !ctx.order_symbol.empty() && ctx.order_side == 1 && ctx.order_price > 0) {
        double order_value = ctx.order_quantity * ctx.order_price;
        double new_conc = order_value / ctx.total_equity;
        auto it = ctx.symbol_positions.find(ctx.order_symbol);
        if (it != ctx.symbol_positions.end()) {
            new_conc += it->second / ctx.total_equity;
        }
        if (new_conc > max_concentration_pct_) {
            return RiskCheckResult::reject(id(), std::string(name()),
                "Pre-trade: concentration exceeded for " + ctx.order_symbol,
                new_conc, max_concentration_pct_);
        }
    }

    // 3. Exposure check
    if (ctx.total_equity > 0) {
        double order_value = 0;
        if (ctx.order_side == 1 && ctx.order_price > 0) {
            order_value = ctx.order_quantity * ctx.order_price;
        }
        double new_exposure = (ctx.total_positions + order_value) / ctx.total_equity;
        if (new_exposure > max_exposure_ratio_) {
            return RiskCheckResult::reject(id(), std::string(name()),
                "Pre-trade: exposure exceeded", new_exposure, max_exposure_ratio_);
        }
    }

    // 4. Single order value check
    if (ctx.order_side == 1 && ctx.order_price > 0) {
        double order_value = ctx.order_quantity * ctx.order_price;
        if (order_value > max_order_value_) {
            return RiskCheckResult::reject(id(), std::string(name()),
                "Pre-trade: order value exceeds limit", order_value, max_order_value_);
        }
    }

    return RiskCheckResult::pass();
}

bool PreTradeCheckRule::validate() const noexcept {
    return max_drawdown_pct_ > 0 && max_drawdown_pct_ <= 1.0
        && max_concentration_pct_ > 0 && max_concentration_pct_ <= 1.0
        && max_exposure_ratio_ > 0
        && max_order_value_ > 0;
}

// ── PortfolioRiskRule ──
RiskCheckResult PortfolioRiskRule::check(const RiskContext& ctx) const {
    if (!enabled_) return RiskCheckResult::pass();

    if (ctx.total_equity <= 0) return RiskCheckResult::pass();

    // Simplified VaR estimate:
    // VaR = total_positions * market_volatility * z_score (use 1.65 for 95% confidence)
    // This is a percentage of equity proxy.
    double var_pct = 0.0;
    if (ctx.total_equity > 0) {
        double z_95 = 1.65;
        double position_ratio = ctx.total_positions / ctx.total_equity;
        double var_estimate = position_ratio * ctx.market_volatility * z_95;
        var_pct = var_estimate;
    }

    if (var_pct > max_var_pct_) {
        return RiskCheckResult::reject(id(), std::string(name()),
            "Portfolio VaR exceeds limit", var_pct, max_var_pct_);
    }
    return RiskCheckResult::pass();
}

bool PortfolioRiskRule::validate() const noexcept {
    return max_var_pct_ > 0 && max_var_pct_ <= 1.0;
}

}  // namespace quant::risk