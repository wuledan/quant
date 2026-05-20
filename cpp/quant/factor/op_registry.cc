// op_registry.cc — Operator registry implementation with built-in operators
#include "cpp/quant/factor/op_registry.h"

#include <cmath>
#include <stdexcept>

namespace quant::factor {

// ── Registry storage ──

std::unordered_map<std::string, OpFactory>& OpRegistry::ops() {
    static std::unordered_map<std::string, OpFactory> instance;
    return instance;
}

void OpRegistry::register_op(const std::string& op_type, OpFactory factory) {
    ops()[op_type] = std::move(factory);
}

OpFactory* OpRegistry::find(const std::string& op_type) {
    auto& m = ops();
    auto it = m.find(op_type);
    return it != m.end() ? &it->second : nullptr;
}

std::vector<std::string> OpRegistry::list_ops() {
    std::vector<std::string> result;
    for (const auto& [k, _] : ops()) result.push_back(k);
    return result;
}

// ── Built-in operator implementations ──

namespace {

// SMA (Simple Moving Average)
FactorComputeFn make_sma(const OpParams& params) {
    int period = static_cast<int>(params.count("period") ? params.at("period") : 20.0);
    return [period](const FactorComputeFnInput& input) -> FactorComputeFnOutput {
        const auto& prices_it = input.find("price");
        if (prices_it == input.end()) return {};
        const auto& prices = prices_it->second;
        size_t n = prices.size();
        if (n == 0) return {};

        FactorComputeFnOutput out;
        std::vector<double> result(n, std::nan(""));
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sum += prices[i];
            if (i >= static_cast<size_t>(period)) {
                sum -= prices[i - period];
                result[i] = sum / period;
            } else if (i == static_cast<size_t>(period - 1)) {
                result[i] = sum / period;
            }
        }
        out["value"] = std::move(result);
        return out;
    };
}

// EMA (Exponential Moving Average)
FactorComputeFn make_ema(const OpParams& params) {
    int period = static_cast<int>(params.count("period") ? params.at("period") : 20.0);
    double alpha = 2.0 / (period + 1.0);
    return [period, alpha](const FactorComputeFnInput& input) -> FactorComputeFnOutput {
        const auto& prices_it = input.find("price");
        if (prices_it == input.end()) return {};
        const auto& prices = prices_it->second;
        size_t n = prices.size();
        if (n == 0) return {};

        FactorComputeFnOutput out;
        std::vector<double> result(n, std::nan(""));
        double ema = prices[0];
        result[0] = ema;
        for (size_t i = 1; i < n; ++i) {
            ema = alpha * prices[i] + (1.0 - alpha) * ema;
            result[i] = ema;
        }
        out["value"] = std::move(result);
        return out;
    };
}

// RSI (Relative Strength Index)
FactorComputeFn make_rsi(const OpParams& params) {
    int period = static_cast<int>(params.count("period") ? params.at("period") : 14.0);
    return [period](const FactorComputeFnInput& input) -> FactorComputeFnOutput {
        const auto& prices_it = input.find("price");
        if (prices_it == input.end()) return {};
        const auto& prices = prices_it->second;
        size_t n = prices.size();
        if (n < 2) return {};

        FactorComputeFnOutput out;
        std::vector<double> result(n, std::nan(""));
        double avg_gain = 0.0, avg_loss = 0.0;

        for (int i = 1; i <= period && i < static_cast<int>(n); ++i) {
            double change = prices[i] - prices[i - 1];
            if (change > 0) avg_gain += change;
            else avg_loss -= change;
        }
        avg_gain /= period;
        avg_loss /= period;

        if (static_cast<size_t>(period) < n) {
            result[period] = avg_loss == 0.0 ? 100.0 : 100.0 - 100.0 / (1.0 + avg_gain / avg_loss);
        }

        for (size_t i = period + 1; i < n; ++i) {
            double change = prices[i] - prices[i - 1];
            double gain = change > 0 ? change : 0.0;
            double loss = change < 0 ? -change : 0.0;
            avg_gain = (avg_gain * (period - 1) + gain) / period;
            avg_loss = (avg_loss * (period - 1) + loss) / period;
            result[i] = avg_loss == 0.0 ? 100.0 : 100.0 - 100.0 / (1.0 + avg_gain / avg_loss);
        }
        out["value"] = std::move(result);
        return out;
    };
}

// CROSS_ABOVE — signal when fast crosses above slow
FactorComputeFn make_cross_above(const OpParams&) {
    return [](const FactorComputeFnInput& input) -> FactorComputeFnOutput {
        const auto& fast_it = input.find("fast");
        const auto& slow_it = input.find("slow");
        if (fast_it == input.end() || slow_it == input.end()) return {};

        const auto& fast = fast_it->second;
        const auto& slow = slow_it->second;
        size_t n = fast.size();
        if (n < 2) return {};

        FactorComputeFnOutput out;
        std::vector<double> result(n, 0.0);
        for (size_t i = 1; i < n; ++i) {
            bool was_below = fast[i - 1] <= slow[i - 1];
            bool is_above = fast[i] > slow[i];
            result[i] = (was_below && is_above) ? 1.0 : 0.0;
        }
        out["value"] = std::move(result);
        return out;
    };
}

// CROSS_BELOW — signal when fast crosses below slow
FactorComputeFn make_cross_below(const OpParams&) {
    return [](const FactorComputeFnInput& input) -> FactorComputeFnOutput {
        const auto& fast_it = input.find("fast");
        const auto& slow_it = input.find("slow");
        if (fast_it == input.end() || slow_it == input.end()) return {};

        const auto& fast = fast_it->second;
        const auto& slow = slow_it->second;
        size_t n = fast.size();
        if (n < 2) return {};

        FactorComputeFnOutput out;
        std::vector<double> result(n, 0.0);
        for (size_t i = 1; i < n; ++i) {
            bool was_above = fast[i - 1] >= slow[i - 1];
            bool is_below = fast[i] < slow[i];
            result[i] = (was_above && is_below) ? -1.0 : 0.0;
        }
        out["value"] = std::move(result);
        return out;
    };
}

// THRESHOLD — signal when value exceeds threshold
FactorComputeFn make_threshold(const OpParams& params) {
    double threshold_val = params.count("threshold") ? params.at("threshold") : 0.0;
    return [threshold_val](const FactorComputeFnInput& input) -> FactorComputeFnOutput {
        const auto& sig_it = input.find("signal");
        if (sig_it == input.end()) return {};

        const auto& values = sig_it->second;
        size_t n = values.size();
        FactorComputeFnOutput out;
        std::vector<double> result(n, 0.0);
        for (size_t i = 0; i < n; ++i) {
            if (values[i] > threshold_val) result[i] = 1.0;
            else if (values[i] < -threshold_val) result[i] = -1.0;
        }
        out["value"] = std::move(result);
        return out;
    };
}

// AND — logical AND of two signals
FactorComputeFn make_and(const OpParams&) {
    return [](const FactorComputeFnInput& input) -> FactorComputeFnOutput {
        const auto& a_it = input.find("a");
        const auto& b_it = input.find("b");
        if (a_it == input.end() || b_it == input.end()) return {};

        const auto& a = a_it->second;
        const auto& b = b_it->second;
        size_t n = a.size();
        FactorComputeFnOutput out;
        std::vector<double> result(n, 0.0);
        for (size_t i = 0; i < n; ++i) {
            result[i] = (a[i] > 0 && b[i] > 0) ? 1.0 : 0.0;
        }
        out["value"] = std::move(result);
        return out;
    };
}

// OR — logical OR of two signals
FactorComputeFn make_or(const OpParams&) {
    return [](const FactorComputeFnInput& input) -> FactorComputeFnOutput {
        const auto& a_it = input.find("a");
        const auto& b_it = input.find("b");
        if (a_it == input.end() || b_it == input.end()) return {};

        const auto& a = a_it->second;
        const auto& b = b_it->second;
        size_t n = a.size();
        FactorComputeFnOutput out;
        std::vector<double> result(n, 0.0);
        for (size_t i = 0; i < n; ++i) {
            result[i] = (a[i] > 0 || b[i] > 0) ? 1.0 : 0.0;
        }
        out["value"] = std::move(result);
        return out;
    };
}

}  // anonymous namespace

// ── Register all built-in operators ──

void OpRegistry::register_all_builtin_ops() {
    register_op("SMA", make_sma);
    register_op("EMA", make_ema);
    register_op("RSI", make_rsi);
    register_op("CROSS_ABOVE", make_cross_above);
    register_op("CROSS_BELOW", make_cross_below);
    register_op("THRESHOLD", make_threshold);
    register_op("AND", make_and);
    register_op("OR", make_or);
}

}  // namespace quant::factor