// strategy_engine.cc — Strategy engine implementation
#include "cpp/quant/strategy/strategy_engine.h"

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/execution/order_manager.h"
#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"
#include "cpp/quant/factor/op_registry.h"
#include "cpp/quant/ir/ir_graph.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/strategy/signal_handler.h"

namespace quant::strategy {

StrategyEngine::StrategyEngine(event::EventBus& bus, storage::StorageEngine& storage)
    : bus_(bus), storage_(storage) {}

bool StrategyEngine::activate(uint64_t strategy_id) {
    auto* entry = registry_.find(strategy_id);
    if (!entry) return false;
    if (active_.count(strategy_id)) return false;

    // Load IR graph
    auto graph = ir::StrategyGraph::load_from_file(entry->graph_path);

    // Build factor DAG
    factor::OpRegistry::register_all_builtin_ops();
    auto factor_reg = std::make_unique<factor::FactorRegistry>();
    auto dag = factor::FactorDAG::from_graph(graph, *factor_reg);
    if (!dag || !dag->is_built()) return false;

    // Create signal handlers from IR
    std::vector<std::unique_ptr<ISignalHandler>> handlers;
    auto order_mgr = std::make_unique<execution::OrderManager>();
    for (const auto& sh_def : graph.signal_handlers) {
        auto handler = SignalHandlerFactory::create(
            sh_def.handler_type, sh_def.params, *order_mgr, bus_);
        if (handler) {
            handlers.push_back(std::move(handler));
        }
    }
    if (handlers.empty()) {
        handlers.push_back(std::make_unique<OrderSignalHandler>(
            OrderSignalHandler::Params{}, *order_mgr, bus_));
    }

    auto active = std::make_unique<ActiveStrategy>();
    active->strategy_id = strategy_id;
    active->factor_registry = std::move(factor_reg);
    active->dag = std::move(dag);
    active->handlers = std::move(handlers);
    active->order_mgr = std::move(order_mgr);

    active_[strategy_id] = std::move(active);
    registry_.update_status(strategy_id, StrategyStatus::kActive);
    return true;
}

bool StrategyEngine::pause(uint64_t strategy_id) {
    if (!active_.count(strategy_id)) return false;
    registry_.update_status(strategy_id, StrategyStatus::kPaused);
    return true;
}

bool StrategyEngine::resume(uint64_t strategy_id) {
    auto* entry = registry_.find(strategy_id);
    if (!entry || entry->status != StrategyStatus::kPaused) return false;
    if (!active_.count(strategy_id)) return false;
    registry_.update_status(strategy_id, StrategyStatus::kActive);
    return true;
}

bool StrategyEngine::deactivate(uint64_t strategy_id) {
    auto it = active_.find(strategy_id);
    if (it == active_.end()) return false;
    active_.erase(it);
    registry_.update_status(strategy_id, StrategyStatus::kDraft);
    return true;
}

bool StrategyEngine::is_active(uint64_t strategy_id) const {
    auto it = active_.find(strategy_id);
    if (it == active_.end()) return false;
    auto* entry = registry_.find(strategy_id);
    return entry && entry->status == StrategyStatus::kActive;
}

StrategyRegistry& StrategyEngine::registry() noexcept { return registry_; }
const StrategyRegistry& StrategyEngine::registry() const noexcept { return registry_; }

}  // namespace quant::strategy
