// strategy_registry.h — Strategy registration and lifecycle management
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace quant::strategy {

enum class StrategyStatus {
    kDraft = 0,
    kActive = 1,
    kPaused = 2,
    kDeleted = 3,
};

struct StrategyEntry {
    uint64_t id = 0;
    std::string name;
    std::string graph_path;
    StrategyStatus status = StrategyStatus::kDraft;
    std::unordered_map<std::string, double> params;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

class StrategyRegistry {
public:
    StrategyRegistry() = default;

    uint64_t register_strategy(const std::string& name,
                                const std::string& graph_path,
                                const std::unordered_map<std::string, double>& params = {});

    bool update_graph_path(uint64_t id, const std::string& graph_path);
    bool update_params(uint64_t id, const std::unordered_map<std::string, double>& params);
    bool update_status(uint64_t id, StrategyStatus status);

    const StrategyEntry* find(uint64_t id) const;
    const StrategyEntry* find_by_name(const std::string& name) const;
    std::vector<StrategyEntry> list_strategies() const;
    std::vector<StrategyEntry> list_by_status(StrategyStatus status) const;

    bool remove_strategy(uint64_t id);
    size_t size() const noexcept;

private:
    std::unordered_map<uint64_t, StrategyEntry> entries_;
    std::unordered_map<std::string, uint64_t> name_index_;
    uint64_t next_id_{1};
};

}  // namespace quant::strategy
