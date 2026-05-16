// factor_registry.cc — FactorRegistry implementation
#include "cpp/quant/factor/factor_registry.h"

#include <algorithm>
#include <chrono>

namespace quant::factor {

FactorId FactorRegistry::register_factor(FactorMeta meta, FactorComputeFn compute_fn) {
    FactorId id = next_id_++;
    if (name_to_id_.contains(meta.name)) {
        FactorId old_id = name_to_id_[meta.name];
        factors_.erase(old_id);
    }
    meta.created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    factors_[id] = {std::move(meta), std::move(compute_fn)};
    name_to_id_[factors_[id].meta.name] = id;
    return id;
}

bool FactorRegistry::unregister_factor(FactorId id) {
    auto it = factors_.find(id);
    if (it == factors_.end()) return false;
    name_to_id_.erase(it->second.meta.name);
    factors_.erase(it);
    return true;
}

const FactorMeta* FactorRegistry::get_meta(FactorId id) const {
    auto it = factors_.find(id);
    return it != factors_.end() ? &it->second.meta : nullptr;
}

const FactorMeta* FactorRegistry::get_meta(std::string_view name) const {
    auto it = name_to_id_.find(std::string(name));
    if (it == name_to_id_.end()) return nullptr;
    return get_meta(it->second);
}

const FactorComputeFn* FactorRegistry::get_compute_fn(FactorId id) const {
    auto it = factors_.find(id);
    return it != factors_.end() ? &it->second.compute_fn : nullptr;
}

FactorId FactorRegistry::find_id(std::string_view name) const {
    auto it = name_to_id_.find(std::string(name));
    return it != name_to_id_.end() ? it->second : 0;
}

std::vector<FactorMeta> FactorRegistry::list_factors() const {
    std::vector<FactorMeta> result;
    result.reserve(factors_.size());
    for (const auto& [id, entry] : factors_) {
        result.push_back(entry.meta);
    }
    return result;
}

bool FactorRegistry::has_factor(std::string_view name) const {
    return name_to_id_.contains(std::string(name));
}

bool FactorRegistry::has_factor(FactorId id) const {
    return factors_.contains(id);
}

}  // namespace quant::factor
