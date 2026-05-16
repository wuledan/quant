// factor_registry.h — Factor metadata registry with dependency declarations
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace quant::factor {

// ── Factor metadata ──
struct FactorMeta {
    std::string name;                    // unique factor name
    std::string description;             // human-readable description
    std::vector<std::string> inputs;     // input factor/data names
    std::vector<std::string> outputs;    // output field names
    uint32_t version{1};                 // version number
    int64_t created_at{0};               // registration timestamp
};

// ── Factor compute function signature ──
// Input: map of named double arrays
// Output: map of named double arrays
using FactorComputeFn = std::function<
    std::unordered_map<std::string, std::vector<double>>(
        const std::unordered_map<std::string, std::vector<double>>&)>;

// ── FactorHandle: unique identifier for a registered factor ──
using FactorId = uint64_t;

// ── FactorRegistry ──
class FactorRegistry {
public:
    FactorRegistry() = default;
    ~FactorRegistry() = default;

    FactorRegistry(const FactorRegistry&) = delete;
    FactorRegistry& operator=(const FactorRegistry&) = delete;

    // Register a factor with its metadata and compute function
    FactorId register_factor(FactorMeta meta, FactorComputeFn compute_fn);

    // Unregister a factor by ID
    bool unregister_factor(FactorId id);

    // Get factor metadata
    const FactorMeta* get_meta(FactorId id) const;
    const FactorMeta* get_meta(std::string_view name) const;

    // Get factor compute function
    const FactorComputeFn* get_compute_fn(FactorId id) const;

    // Lookup factor ID by name
    FactorId find_id(std::string_view name) const;

    // List all registered factors
    std::vector<FactorMeta> list_factors() const;

    // Number of registered factors
    size_t size() const noexcept { return factors_.size(); }

    // Check if factor exists
    bool has_factor(std::string_view name) const;
    bool has_factor(FactorId id) const;

private:
    struct FactorEntry {
        FactorMeta meta;
        FactorComputeFn compute_fn;
    };

    std::unordered_map<FactorId, FactorEntry> factors_;
    std::unordered_map<std::string, FactorId> name_to_id_;
    FactorId next_id_{1};
};

}  // namespace quant::factor
