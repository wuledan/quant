// factor_dag.h — DAG-based factor dependency management
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cpp/quant/factor/factor_registry.h"

namespace quant::factor {

// ── DAG validation result ──
struct DAGValidationResult {
    bool valid{true};
    std::string message;
    std::vector<FactorId> cycle_path;  // factors forming a cycle (if any)
};

// ── FactorDAG: builds and validates dependency graph ──
class FactorDAG {
public:
    explicit FactorDAG(const FactorRegistry* registry);
    ~FactorDAG() = default;

    FactorDAG(const FactorDAG&) = delete;
    FactorDAG& operator=(const FactorDAG&) = delete;

    // Build/rebuild the DAG from registry dependencies
    void build();

    // Validate the DAG (cycle detection)
    DAGValidationResult validate() const;

    // Get topological sort order (computation sequence)
    std::vector<FactorId> topological_sort() const;

    // Group factors into parallel execution levels
    // Each level can be executed concurrently
    std::vector<std::vector<FactorId>> parallel_levels() const;

    // Get dependencies for a factor
    std::vector<FactorId> get_dependencies(FactorId id) const;

    // Get dependents (reverse dependencies) for a factor
    std::vector<FactorId> get_dependents(FactorId id) const;

    // Clear the DAG
    void clear();

    // Check if DAG is built
    bool is_built() const noexcept { return built_; }

private:
    // DFS-based topological sort with cycle detection
    bool dfs_topo(FactorId id,
                  std::unordered_set<FactorId>& visited,
                  std::unordered_set<FactorId>& in_stack,
                  std::vector<FactorId>& order,
                  std::vector<FactorId>& cycle_path) const;

    const FactorRegistry* registry_;
    bool built_{false};

    // Adjacency lists
    std::unordered_map<FactorId, std::vector<FactorId>> deps_;     // factor → deps
    std::unordered_map<FactorId, std::vector<FactorId>> dependents_; // factor → dependents
};

}  // namespace quant::factor
