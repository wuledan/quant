// op_registry.h — Operator registry mapping op_type strings to compute function factories
//
// When the IR loader encounters a NodeDef with op_type="SMA" and params={period:5},
// it calls OpRegistry::find("SMA") to get a factory, then passes params to create
// the actual FactorComputeFn for that specific node instance.

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/factor/factor_registry.h"

namespace quant::factor {

// ── OpFactory: creates a FactorComputeFn from IR params ──
using OpParams = std::unordered_map<std::string, double>;
using FactorComputeFnInput = std::unordered_map<std::string, std::vector<double>>;
using FactorComputeFnOutput = std::unordered_map<std::string, std::vector<double>>;
using OpFactory = std::function<FactorComputeFn(const OpParams& params)>;

// ── OpRegistry: maps op_type strings to factories ──
class OpRegistry {
public:
    // Register an operator factory
    static void register_op(const std::string& op_type, OpFactory factory);

    // Find factory by op_type. Returns nullptr if not found.
    static OpFactory* find(const std::string& op_type);

    // List all registered op_types
    static std::vector<std::string> list_ops();

    // Register all built-in operators (SMA, EMA, RSI, MACD, BOLL,
    // CROSS_ABOVE, CROSS_BELOW, THRESHOLD, AND, OR, NOT)
    static void register_all_builtin_ops();

private:
    static std::unordered_map<std::string, OpFactory>& ops();
};

}  // namespace quant::factor