// strategy_engine.h — Strategy execution engine: loads IR, builds DAG, runs
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "cpp/quant/execution/order_manager.h"
#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"
#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/strategy/signal_handler.h"
#include "cpp/quant/strategy/strategy_runner.h"
#include "cpp/quant/strategy/strategy_registry.h"

namespace quant::event { class EventBus; }
namespace quant::storage { class StorageEngine; }

namespace quant::strategy {

struct LiveStatus {
    bool is_running;
    std::optional<SignalResult> latest_signal;
    std::unordered_map<std::string, double> factors;
};

class StrategyEngine {
public:
    StrategyEngine(event::EventBus& bus, storage::StorageEngine& storage);

    // Legacy lifecycle
    bool activate(uint64_t strategy_id);
    bool pause(uint64_t strategy_id);
    bool resume(uint64_t strategy_id);
    bool deactivate(uint64_t strategy_id);
    bool is_active(uint64_t strategy_id) const;

    // Real-time live strategy management
    std::optional<SignalResult> start_live(uint64_t strategy_id,
                                            const ir::StrategyGraph& graph);
    void stop_live(uint64_t strategy_id);
    LiveStatus live_status(uint64_t strategy_id) const;
    std::vector<uint64_t> live_strategies() const;

    StrategyRegistry& registry() noexcept;
    const StrategyRegistry& registry() const noexcept;

private:
    event::EventBus& bus_;
    storage::StorageEngine& storage_;
    StrategyRegistry registry_;

    // Legacy active strategies
    struct ActiveStrategy {
        uint64_t strategy_id;
        std::unique_ptr<factor::FactorRegistry> factor_registry;
        std::unique_ptr<factor::FactorDAG> dag;
        std::vector<std::unique_ptr<ISignalHandler>> handlers;
        std::unique_ptr<execution::OrderManager> order_mgr;
    };
    std::unordered_map<uint64_t, std::unique_ptr<ActiveStrategy>> active_;

    // Real-time live runners
    std::unordered_map<uint64_t, std::unique_ptr<StrategyRunner>> live_runners_;
};

}  // namespace quant::strategy
