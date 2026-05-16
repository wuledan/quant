// built_in_factors.h — Common built-in technical indicator factors
// MA, EMA, RSI, MACD, Bollinger Bands
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/factor/factor_computer.h"

namespace quant::factor {

// ── BuiltInFactors: register all standard technical indicators ──
class BuiltInFactors {
public:
    // Register all built-in factors into a FactorRegistry
    static void register_all(FactorRegistry& registry);

    // Individual registration helpers (useful for selective registration)
    static void register_ma(FactorRegistry& registry);
    static void register_ema(FactorRegistry& registry);
    static void register_rsi(FactorRegistry& registry);
    static void register_macd(FactorRegistry& registry);
    static void register_boll(FactorRegistry& registry);

    // ── Static compute functions (usable directly without registry) ──

    // Simple Moving Average
    static std::vector<double> ma(const std::vector<double>& values, int period);

    // Exponential Moving Average
    static std::vector<double> ema(const std::vector<double>& values, int period);

    // Relative Strength Index
    static std::vector<double> rsi(const std::vector<double>& values, int period);

    // MACD (returns: {macd_line, signal_line, histogram})
    struct MACDResult {
        std::vector<double> macd_line;
        std::vector<double> signal_line;
        std::vector<double> histogram;
    };
    static MACDResult macd(const std::vector<double>& values,
                            int fast_period = 12,
                            int slow_period = 26,
                            int signal_period = 9);

    // Bollinger Bands (returns: {upper, middle, lower})
    struct BollingerResult {
        std::vector<double> upper;
        std::vector<double> middle;
        std::vector<double> lower;
    };
    static BollingerResult bollinger(const std::vector<double>& values,
                                      int period = 20,
                                      double num_std = 2.0);
};

}  // namespace quant::factor
