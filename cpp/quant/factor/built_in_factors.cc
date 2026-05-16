// built_in_factors.cc — BuiltInFactors implementation
#include "cpp/quant/factor/built_in_factors.h"

#include <cmath>
#include <limits>

namespace quant::factor {

// ========================================================================
// Registration
// ========================================================================

void BuiltInFactors::register_all(FactorRegistry& registry) {
    register_ma(registry);
    register_ema(registry);
    register_rsi(registry);
    register_macd(registry);
    register_boll(registry);
}

void BuiltInFactors::register_ma(FactorRegistry& registry) {
    FactorMeta meta;
    meta.name = "MA";
    meta.description = "Simple Moving Average";
    meta.inputs = {"close"};
    meta.outputs = {"ma"};
    meta.version = 1;

    registry.register_factor(std::move(meta),
        [](const std::unordered_map<std::string, std::vector<double>>& inputs) {
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& close = inputs.at("close");
            // Default period=20, compute all periods 1..n (prefix averages)
            out["ma"] = ma(close, 20);
            return out;
        });
}

void BuiltInFactors::register_ema(FactorRegistry& registry) {
    FactorMeta meta;
    meta.name = "EMA";
    meta.description = "Exponential Moving Average";
    meta.inputs = {"close"};
    meta.outputs = {"ema"};
    meta.version = 1;

    registry.register_factor(std::move(meta),
        [](const std::unordered_map<std::string, std::vector<double>>& inputs) {
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& close = inputs.at("close");
            out["ema"] = ema(close, 20);
            return out;
        });
}

void BuiltInFactors::register_rsi(FactorRegistry& registry) {
    FactorMeta meta;
    meta.name = "RSI";
    meta.description = "Relative Strength Index";
    meta.inputs = {"close"};
    meta.outputs = {"rsi"};
    meta.version = 1;

    registry.register_factor(std::move(meta),
        [](const std::unordered_map<std::string, std::vector<double>>& inputs) {
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& close = inputs.at("close");
            out["rsi"] = rsi(close, 14);
            return out;
        });
}

void BuiltInFactors::register_macd(FactorRegistry& registry) {
    FactorMeta meta;
    meta.name = "MACD";
    meta.description = "Moving Average Convergence Divergence";
    meta.inputs = {"close"};
    meta.outputs = {"macd_line", "signal_line", "macd_histogram"};
    meta.version = 1;

    registry.register_factor(std::move(meta),
        [](const std::unordered_map<std::string, std::vector<double>>& inputs) {
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& close = inputs.at("close");
            auto result = macd(close, 12, 26, 9);
            out["macd_line"] = std::move(result.macd_line);
            out["signal_line"] = std::move(result.signal_line);
            out["macd_histogram"] = std::move(result.histogram);
            return out;
        });
}

void BuiltInFactors::register_boll(FactorRegistry& registry) {
    FactorMeta meta;
    meta.name = "BOLL";
    meta.description = "Bollinger Bands";
    meta.inputs = {"close"};
    meta.outputs = {"boll_upper", "boll_middle", "boll_lower"};
    meta.version = 1;

    registry.register_factor(std::move(meta),
        [](const std::unordered_map<std::string, std::vector<double>>& inputs) {
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& close = inputs.at("close");
            auto result = bollinger(close, 20, 2.0);
            out["boll_upper"] = std::move(result.upper);
            out["boll_middle"] = std::move(result.middle);
            out["boll_lower"] = std::move(result.lower);
            return out;
        });
}

// ========================================================================
// MA — Simple Moving Average
// ========================================================================

std::vector<double> BuiltInFactors::ma(
    const std::vector<double>& values, int period) {
    if (values.empty() || period <= 0) return {};

    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());

    double sum = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        sum += values[i];
        if (static_cast<int>(i) >= period) {
            sum -= values[i - period];
        }
        if (static_cast<int>(i) >= period - 1) {
            result[i] = sum / period;
        }
    }
    return result;
}

// ========================================================================
// EMA — Exponential Moving Average
// ========================================================================

std::vector<double> BuiltInFactors::ema(
    const std::vector<double>& values, int period) {
    if (values.empty() || period <= 0) return {};

    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    double multiplier = 2.0 / (period + 1);

    // Start with SMA as first EMA value
    double sum = 0.0;
    for (int i = 0; i < period && i < static_cast<int>(values.size()); ++i) {
        sum += values[i];
    }
    if (static_cast<int>(values.size()) >= period) {
        result[period - 1] = sum / period;
        for (size_t i = period; i < values.size(); ++i) {
            result[i] = (values[i] - result[i - 1]) * multiplier + result[i - 1];
        }
    }
    return result;
}

// ========================================================================
// RSI — Relative Strength Index
// ========================================================================

std::vector<double> BuiltInFactors::rsi(
    const std::vector<double>& values, int period) {
    if (values.size() < static_cast<size_t>(period + 1) || period <= 0) return {};

    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());

    double avg_gain = 0.0, avg_loss = 0.0;

    // First average gain/loss
    for (size_t i = 1; i <= static_cast<size_t>(period); ++i) {
        double diff = values[i] - values[i - 1];
        if (diff > 0) avg_gain += diff;
        else avg_loss -= diff;
    }
    avg_gain /= period;
    avg_loss /= period;

    if (avg_loss == 0.0) {
        result[period] = 100.0;
    } else {
        double rs = avg_gain / avg_loss;
        result[period] = 100.0 - 100.0 / (1.0 + rs);
    }

    // Subsequent values using smoothed method
    for (size_t i = period + 1; i < values.size(); ++i) {
        double diff = values[i] - values[i - 1];
        double gain = (diff > 0) ? diff : 0.0;
        double loss = (diff < 0) ? -diff : 0.0;

        avg_gain = (avg_gain * (period - 1) + gain) / period;
        avg_loss = (avg_loss * (period - 1) + loss) / period;

        if (avg_loss == 0.0) {
            result[i] = 100.0;
        } else {
            double rs = avg_gain / avg_loss;
            result[i] = 100.0 - 100.0 / (1.0 + rs);
        }
    }

    return result;
}

// ========================================================================
// MACD
// ========================================================================

BuiltInFactors::MACDResult BuiltInFactors::macd(
    const std::vector<double>& values,
    int fast_period, int slow_period, int signal_period) {
    MACDResult result;
    if (values.empty() || fast_period <= 0 || slow_period <= 0 || signal_period <= 0) {
        return result;
    }

    // Compute fast and slow EMAs
    auto fast_ema = ema(values, fast_period);
    auto slow_ema = ema(values, slow_period);

    // MACD line = fast_ema - slow_ema
    result.macd_line.resize(values.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < values.size(); ++i) {
        if (!std::isnan(fast_ema[i]) && !std::isnan(slow_ema[i])) {
            result.macd_line[i] = fast_ema[i] - slow_ema[i];
        }
    }

    // Signal line = EMA of MACD line (skip NaN prefix)
    // Find first non-NaN index in MACD line
    size_t first_valid = 0;
    for (; first_valid < result.macd_line.size(); ++first_valid) {
        if (!std::isnan(result.macd_line[first_valid])) break;
    }
    if (first_valid < result.macd_line.size()) {
        // Extract valid portion and compute EMA
        std::vector<double> valid_macd(result.macd_line.begin() + first_valid,
                                        result.macd_line.end());
        auto sig = ema(valid_macd, signal_period);
        result.signal_line.resize(values.size(), std::numeric_limits<double>::quiet_NaN());
        for (size_t i = 0; i < sig.size(); ++i) {
            result.signal_line[first_valid + i] = sig[i];
        }
    }

    // Histogram = MACD line - Signal line
    result.histogram.resize(values.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < values.size(); ++i) {
        if (!std::isnan(result.macd_line[i]) && !std::isnan(result.signal_line[i])) {
            result.histogram[i] = result.macd_line[i] - result.signal_line[i];
        }
    }

    return result;
}

// ========================================================================
// Bollinger Bands
// ========================================================================

BuiltInFactors::BollingerResult BuiltInFactors::bollinger(
    const std::vector<double>& values,
    int period, double num_std) {
    BollingerResult result;
    if (values.empty() || period <= 0) return result;

    auto middle = ma(values, period);

    result.upper.resize(values.size(), std::numeric_limits<double>::quiet_NaN());
    result.middle = std::move(middle);
    result.lower.resize(values.size(), std::numeric_limits<double>::quiet_NaN());

    for (size_t i = static_cast<size_t>(period - 1); i < values.size(); ++i) {
        // Compute standard deviation over the window
        double sum = 0.0, mean = result.middle[i];
        for (int j = 0; j < period; ++j) {
            sum += (values[i - j] - mean) * (values[i - j] - mean);
        }
        double stddev = std::sqrt(sum / period);

        result.upper[i] = mean + num_std * stddev;
        result.lower[i] = mean - num_std * stddev;
    }

    return result;
}

}  // namespace quant::factor
