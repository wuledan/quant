// factor_dag.cc — FactorDAG implementation
#include "cpp/quant/factor/factor_dag.h"

#include <algorithm>
#include <queue>
#include <stdexcept>

#include "cpp/quant/factor/op_registry.h"
#include "cpp/quant/ir/ir_graph.h"

namespace quant::factor {

FactorDAG::FactorDAG(const FactorRegistry* registry)
    : registry_(registry) {}

void FactorDAG::add_dependency(FactorId dependent, FactorId dependency) {
    deps_[dependent].push_back(dependency);
    dependents_[dependency].push_back(dependent);
}

// ── IR-based construction ──

std::unique_ptr<FactorDAG> FactorDAG::from_graph(
    const quant::ir::StrategyGraph& graph,
    FactorRegistry& registry) {

    // Register built-in ops if not already done
    OpRegistry::register_all_builtin_ops();

    // Map IR node IDs to FactorRegistry IDs
    std::unordered_map<std::string, FactorId> node_to_factor_id;

    // 1. Register each IR node as a factor
    for (const auto& node : graph.nodes) {
        auto* factory = OpRegistry::find(node.op_type);
        if (!factory) {
            throw std::runtime_error(
                "Unknown op_type: " + node.op_type +
                " for node: " + node.id);
        }

        // Create compute function from factory with node params
        auto compute_fn = (*factory)(node.params);

        // Build FactorMeta
        FactorMeta meta;
        meta.name = node.id;
        meta.description = "IR node: " + node.op_type;

        // Set inputs from edges (will be filled after edge processing)
        // For now, use the port names from the IR node
        for (const auto& [port_name, port_def] : node.inputs) {
            meta.inputs.push_back(port_def.source);
        }
        for (const auto& [port_name, port_def] : node.outputs) {
            meta.outputs.push_back(node.id + "." + port_name);
        }

        FactorId id = registry.register_factor(std::move(meta), std::move(compute_fn));
        node_to_factor_id[node.id] = id;
    }

    // 2. Build the DAG
    auto dag = std::make_unique<FactorDAG>(&registry);

    // Set up dependency edges from IR edges
    for (const auto& edge : graph.edges) {
        auto from_id_it = node_to_factor_id.find(edge.from_node);
        auto to_id_it = node_to_factor_id.find(edge.to_node);
        if (from_id_it == node_to_factor_id.end() ||
            to_id_it == node_to_factor_id.end()) {
            throw std::runtime_error(
                "Edge references unknown node: " +
                edge.from_node + " → " + edge.to_node);
        }
        dag->add_dependency(to_id_it->second, from_id_it->second);
    }

    // Ensure all nodes have entries in deps/dependents maps
    for (const auto& [_, id] : node_to_factor_id) {
        if (!dag->deps_.contains(id)) dag->deps_[id] = {};
        if (!dag->dependents_.contains(id)) dag->dependents_[id] = {};
    }

    dag->built_ = true;
    return dag;
}

void FactorDAG::build() {
    clear();

    auto factors = registry_->list_factors();
    for (const auto& meta : factors) {
        FactorId id = registry_->find_id(meta.name);
        for (const auto& input_name : meta.inputs) {
            FactorId dep_id = registry_->find_id(input_name);
            if (dep_id != 0 && dep_id != id) {
                deps_[id].push_back(dep_id);
                dependents_[dep_id].push_back(id);
            }
        }
        if (!deps_.contains(id)) deps_[id] = {};
        if (!dependents_.contains(id)) dependents_[id] = {};
    }
    built_ = true;
}

DAGValidationResult FactorDAG::validate() const {
    DAGValidationResult result;
    std::unordered_set<FactorId> visited;
    std::unordered_set<FactorId> in_stack;
    std::vector<FactorId> order;

    auto factors = registry_->list_factors();
    for (const auto& meta : factors) {
        FactorId id = registry_->find_id(meta.name);
        if (!visited.contains(id)) {
            std::vector<FactorId> cycle_path;
            if (!dfs_topo(id, visited, in_stack, order, cycle_path)) {
                result.valid = false;
                result.message = "Cycle detected in factor dependency graph";
                result.cycle_path = std::move(cycle_path);
                return result;
            }
        }
    }
    result.message = "DAG is valid";
    return result;
}

std::vector<FactorId> FactorDAG::topological_sort() const {
    std::vector<FactorId> order;
    std::unordered_set<FactorId> visited;
    std::unordered_set<FactorId> in_stack;
    std::vector<FactorId> cycle_path;

    auto factors = registry_->list_factors();
    for (const auto& meta : factors) {
        FactorId id = registry_->find_id(meta.name);
        if (!visited.contains(id)) {
            dfs_topo(id, visited, in_stack, order, cycle_path);
        }
    }
    return order;
}

std::vector<std::vector<FactorId>> FactorDAG::parallel_levels() const {
    std::unordered_map<FactorId, int> in_degree;
    for (const auto& [id, _] : deps_) {
        in_degree[id] = static_cast<int>(deps_.at(id).size());
    }

    std::queue<FactorId> q;
    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) q.push(id);
    }

    std::vector<std::vector<FactorId>> levels;
    while (!q.empty()) {
        std::vector<FactorId> current_level;
        size_t level_size = q.size();
        for (size_t i = 0; i < level_size; ++i) {
            FactorId id = q.front(); q.pop();
            current_level.push_back(id);
            if (dependents_.contains(id)) {
                for (auto dep_id : dependents_.at(id)) {
                    in_degree[dep_id]--;
                    if (in_degree[dep_id] == 0) {
                        q.push(dep_id);
                    }
                }
            }
        }
        levels.push_back(std::move(current_level));
    }

    return levels;
}

std::vector<FactorId> FactorDAG::get_dependencies(FactorId id) const {
    auto it = deps_.find(id);
    return it != deps_.end() ? it->second : std::vector<FactorId>{};
}

std::vector<FactorId> FactorDAG::get_dependents(FactorId id) const {
    auto it = dependents_.find(id);
    return it != dependents_.end() ? it->second : std::vector<FactorId>{};
}

void FactorDAG::clear() {
    deps_.clear();
    dependents_.clear();
    built_ = false;
}

bool FactorDAG::dfs_topo(FactorId id,
                          std::unordered_set<FactorId>& visited,
                          std::unordered_set<FactorId>& in_stack,
                          std::vector<FactorId>& order,
                          std::vector<FactorId>& cycle_path) const {
    visited.insert(id);
    in_stack.insert(id);
    cycle_path.push_back(id);

    auto it = deps_.find(id);
    if (it != deps_.end()) {
        for (auto dep_id : it->second) {
            if (!visited.contains(dep_id)) {
                if (!dfs_topo(dep_id, visited, in_stack, order, cycle_path)) {
                    return false;
                }
            } else if (in_stack.contains(dep_id)) {
                cycle_path.push_back(dep_id);
                return false;
            }
        }
    }

    in_stack.erase(id);
    cycle_path.pop_back();
    order.push_back(id);
    return true;
}

}  // namespace quant::factor
