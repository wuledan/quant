// strategy_engine.h — Strategy execution engine: loads IR, builds DAG, runs
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "cpp/quant/execution/order_manager.h"
#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"
#include "cpp/quant/strategy/signal_handler.h"
#include "cpp/quant/strategy/strategy_registry.h"

namespace quant::event { class EventBus; }
namespace quant::storage { class StorageEngine; }

namespace quant::strategy {

class StrategyEngine {
public:
    StrategyEngine(event::EventBus& bus, storage::StorageEngine& storage);

    bool activate(uint64_t strategy_id);
    bool pause(uint64_t strategy_id);
    bool resume(uint64_t strategy_id);
    bool deactivate(uint64_t strategy_id);
    bool is_active(uint64_t strategy_id) const;

    StrategyRegistry& registry() noexcept;
    const StrategyRegistry& registry() const noexcept;

private:
    struct ActiveStrategy {
        uint64_t strategy_id;
        std::unique_ptr<factor::FactorRegistry> factor_registry;
        std::unique_ptr<factor::FactorDAG> dag;
        std::vector<std::unique_ptr<ISignalHandler>> handlers;
        std::unique_ptr<execution::OrderManager> order_mgr;
    };

    event::EventBus& bus_;
    storage::StorageEngine& storage_;
    StrategyRegistry registry_;
    std::unordered_map<uint64_t, std::unique_ptr<ActiveStrategy>> active_;
};

}  // namespace quant::strategy
