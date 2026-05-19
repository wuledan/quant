// factor_computer.h — Factor computation engine with incremental/vectorized compute
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"

namespace quant::factor {

// ── Compute result ──
struct ComputeResult {
    bool success{false};
    std::string error_msg;
    std::unordered_map<std::string, std::vector<double>> outputs;
    int64_t compute_time_ns{0};  // nanoseconds
};

// ── FactorComputer ──
class FactorComputer {
public:
    FactorComputer(std::unique_ptr<FactorRegistry> registry,
                    std::unique_ptr<FactorDAG> dag);
    ~FactorComputer();

    FactorComputer(const FactorComputer&) = delete;
    FactorComputer& operator=(const FactorComputer&) = delete;

    // Full compute: evaluate all factors topologically
    ComputeResult compute_all(
        const std::unordered_map<std::string, std::vector<double>>& input_data);

    // Compute a specific factor and its dependencies
    ComputeResult compute_factor(
        std::string_view factor_name,
        const std::unordered_map<std::string, std::vector<double>>& input_data);

    // Incremental update: only recompute affected factors
    ComputeResult increment(
        std::string_view changed_input,
        const std::unordered_map<std::string, std::vector<double>>& new_data);

    // Get cached result for a factor
    const std::vector<double>* get_cached(std::string_view factor_name) const;

    // Invalidate cache for a factor (and its dependents)
    void invalidate(std::string_view factor_name);

    // Clear all cached results
    void clear_cache();

    // Access registry/DAG
    FactorRegistry& registry() noexcept { return *registry_; }
    FactorDAG& dag() noexcept { return *dag_; }

private:
    ComputeResult compute_factor_impl(
        FactorId id,
        const std::unordered_map<std::string, std::vector<double>>& input_data,
        std::unordered_set<FactorId>* computed);

    std::unique_ptr<FactorRegistry> registry_;
    std::unique_ptr<FactorDAG> dag_;
    std::unordered_map<std::string, std::vector<double>> cache_;
    mutable std::mutex cache_mutex_;  // protects cache_ for concurrent access
};

}  // namespace quant::factor
