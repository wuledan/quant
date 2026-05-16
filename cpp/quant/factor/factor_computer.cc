// factor_computer.cc — FactorComputer implementation
#include "cpp/quant/factor/factor_computer.h"

#include <algorithm>
#include <chrono>

namespace quant::factor {

FactorComputer::FactorComputer(
    std::unique_ptr<FactorRegistry> registry,
    std::unique_ptr<FactorDAG> dag)
    : registry_(std::move(registry))
    , dag_(std::move(dag)) {
    dag_->build();
}

FactorComputer::~FactorComputer() = default;

ComputeResult FactorComputer::compute_all(
    const std::unordered_map<std::string, std::vector<double>>& input_data) {
    ComputeResult result;
    auto start = std::chrono::high_resolution_clock::now();

    dag_->build();
    auto validation = dag_->validate();
    if (!validation.valid) {
        result.success = false;
        result.error_msg = "DAG validation failed: " + validation.message;
        return result;
    }

    auto order = dag_->topological_sort();
    std::unordered_set<FactorId> computed;

    for (auto id : order) {
        auto sub_result = compute_factor_impl(id, input_data, &computed);
        if (!sub_result.success) {
            result.success = false;
            result.error_msg = sub_result.error_msg;
            return result;
        }
        for (auto& [k, v] : sub_result.outputs) {
            result.outputs[k] = std::move(v);
        }
    }

    result.success = true;
    auto end = std::chrono::high_resolution_clock::now();
    result.compute_time_ns = std::chrono::duration_cast<
        std::chrono::nanoseconds>(end - start).count();
    return result;
}

ComputeResult FactorComputer::compute_factor(
    std::string_view factor_name,
    const std::unordered_map<std::string, std::vector<double>>& input_data) {
    FactorId id = registry_->find_id(factor_name);
    if (id == 0) {
        ComputeResult r;
        r.success = false;
        r.error_msg = std::string("Factor not found: ") + std::string(factor_name);
        return r;
    }

    std::unordered_set<FactorId> computed;
    return compute_factor_impl(id, input_data, &computed);
}

ComputeResult FactorComputer::increment(
    std::string_view changed_input,
    const std::unordered_map<std::string, std::vector<double>>& new_data) {
    ComputeResult result;
    auto start = std::chrono::high_resolution_clock::now();

    // Update cache with new input
    auto data_it = new_data.find(std::string(changed_input));
    if (data_it != new_data.end()) {
        cache_[std::string(changed_input)] = data_it->second;
    }

    // Find affected factors (those that depend on the changed input directly or indirectly)
    std::unordered_set<FactorId> affected;
    FactorId input_id = registry_->find_id(changed_input);
    if (input_id != 0) {
        auto deps = dag_->get_dependents(input_id);
        affected.insert(deps.begin(), deps.end());
    } else {
        // changed_input is raw data — find factors that list it as an input
        auto all_factors = registry_->list_factors();
        for (const auto& meta : all_factors) {
            for (const auto& inp : meta.inputs) {
                if (inp == changed_input) {
                    affected.insert(registry_->find_id(meta.name));
                    break;
                }
            }
        }
    }
    if (affected.empty()) return result;

    std::unordered_set<FactorId> computed;

    // Process affected factors in topological order
    auto order = dag_->topological_sort();
    for (auto id : order) {
        if (affected.end() !=
            std::find(affected.begin(), affected.end(), id)) {
            auto sub_result = compute_factor_impl(id, new_data, &computed);
            if (!sub_result.success) {
                result.success = false;
                result.error_msg = sub_result.error_msg;
                return result;
            }
            for (auto& [k, v] : sub_result.outputs) {
                result.outputs[k] = std::move(v);
            }
        }
    }

    result.success = true;
    auto end = std::chrono::high_resolution_clock::now();
    result.compute_time_ns = std::chrono::duration_cast<
        std::chrono::nanoseconds>(end - start).count();
    return result;
}

const std::vector<double>* FactorComputer::get_cached(
    std::string_view factor_name) const {
    auto it = cache_.find(std::string(factor_name));
    return it != cache_.end() ? &it->second : nullptr;
}

void FactorComputer::invalidate(std::string_view factor_name) {
    cache_.erase(std::string(factor_name));
    // Also invalidate dependents
    FactorId id = registry_->find_id(factor_name);
    if (id != 0) {
        auto deps = dag_->get_dependents(id);
        for (auto dep_id : deps) {
            auto* meta = registry_->get_meta(dep_id);
            if (meta) cache_.erase(meta->name);
        }
    }
}

void FactorComputer::clear_cache() {
    cache_.clear();
}

ComputeResult FactorComputer::compute_factor_impl(
    FactorId id,
    const std::unordered_map<std::string, std::vector<double>>& input_data,
    std::unordered_set<FactorId>* computed) {
    ComputeResult result;

    if (computed->contains(id)) {
        // Already computed, return from cache
        auto* meta = registry_->get_meta(id);
        if (meta) {
            auto* cached = get_cached(meta->name);
            if (cached) {
                result.success = true;
                result.outputs[meta->name] = *cached;
                return result;
            }
        }
    }

    auto* meta = registry_->get_meta(id);
    if (!meta) {
        result.success = false;
        result.error_msg = "Factor ID not found: " + std::to_string(id);
        return result;
    }

    auto* compute_fn = registry_->get_compute_fn(id);
    if (!compute_fn) {
        result.success = false;
        result.error_msg = "No compute function for factor: " + meta->name;
        return result;
    }

    // Gather input data
    std::unordered_map<std::string, std::vector<double>> resolved_inputs;
    for (const auto& input_name : meta->inputs) {
        // Check cache first
        auto cache_it = cache_.find(input_name);
        if (cache_it != cache_.end()) {
            resolved_inputs[input_name] = cache_it->second;
            continue;
        }
        // Check provided input data
        auto data_it = input_data.find(input_name);
        if (data_it != input_data.end()) {
            resolved_inputs[input_name] = data_it->second;
            continue;
        }
        // Try to compute the input factor
        FactorId input_id = registry_->find_id(input_name);
        if (input_id != 0 && input_id != id) {
            auto dep_result = compute_factor_impl(input_id, input_data, computed);
            if (dep_result.success) {
                for (auto& [k, v] : dep_result.outputs) {
                    resolved_inputs[k] = v;
                }
            }
        }
        if (!resolved_inputs.contains(input_name)) {
            result.success = false;
            result.error_msg = "Missing input data for factor '" + meta->name
                              + "': '" + input_name + "'";
            return result;
        }
    }

    // Execute compute function
    try {
        auto outputs = (*compute_fn)(resolved_inputs);
        computed->insert(id);
        for (const auto& [k, v] : outputs) {
            cache_[k] = v;
            result.outputs[k] = v;
        }
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_msg = "Compute error for '" + meta->name + "': " + e.what();
    }

    return result;
}

}  // namespace quant::factor
