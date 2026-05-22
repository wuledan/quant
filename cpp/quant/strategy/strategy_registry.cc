// strategy_registry.cc — Strategy registry implementation
#include "cpp/quant/strategy/strategy_registry.h"

#include <chrono>

namespace quant::strategy {

uint64_t StrategyRegistry::register_strategy(
    const std::string& name,
    const std::string& graph_path,
    const std::unordered_map<std::string, double>& params) {
    uint64_t id = next_id_++;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    int64_t ts = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    StrategyEntry entry;
    entry.id = id;
    entry.name = name;
    entry.graph_path = graph_path;
    entry.params = params;
    entry.status = StrategyStatus::kDraft;
    entry.created_at = ts;
    entry.updated_at = ts;

    entries_[id] = std::move(entry);
    name_index_[name] = id;
    return id;
}

bool StrategyRegistry::update_graph_path(uint64_t id, const std::string& graph_path) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    it->second.graph_path = graph_path;
    return true;
}

bool StrategyRegistry::update_params(uint64_t id,
                                      const std::unordered_map<std::string, double>& params) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    it->second.params = params;
    return true;
}

bool StrategyRegistry::update_status(uint64_t id, StrategyStatus status) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    it->second.status = status;
    return true;
}

const StrategyEntry* StrategyRegistry::find(uint64_t id) const {
    auto it = entries_.find(id);
    return (it != entries_.end()) ? &it->second : nullptr;
}

const StrategyEntry* StrategyRegistry::find_by_id(uint64_t id) const {
    return find(id);
}

const StrategyEntry* StrategyRegistry::find_by_name(const std::string& name) const {
    auto it = name_index_.find(name);
    if (it == name_index_.end()) return nullptr;
    return find(it->second);
}

std::vector<StrategyEntry> StrategyRegistry::list_strategies() const {
    std::vector<StrategyEntry> result;
    result.reserve(entries_.size());
    for (const auto& [_, entry] : entries_) {
        result.push_back(entry);
    }
    return result;
}

std::vector<StrategyEntry> StrategyRegistry::list_by_status(StrategyStatus status) const {
    std::vector<StrategyEntry> result;
    for (const auto& [_, entry] : entries_) {
        if (entry.status == status) result.push_back(entry);
    }
    return result;
}

bool StrategyRegistry::remove_strategy(uint64_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    name_index_.erase(it->second.name);
    entries_.erase(it);
    return true;
}

size_t StrategyRegistry::size() const noexcept {
    return entries_.size();
}

}  // namespace quant::strategy
