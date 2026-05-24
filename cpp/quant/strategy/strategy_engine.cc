// strategy_engine.cc — Strategy engine implementation
#include "cpp/quant/strategy/strategy_engine.h"
#include <iostream>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/execution/order_manager.h"
#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"
#include "cpp/quant/factor/op_registry.h"
#include "cpp/quant/infra/logging/logger.h"
#include "cpp/quant/ir/ir_graph.h"
#include <folly/futures/Future.h>

#include "cpp/quant/network/global_executor.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/strategy/signal_handler.h"

namespace quant::strategy {
using infra::default_logger;

StrategyEngine::StrategyEngine(event::EventBus& bus, storage::StorageEngine& storage)
    : bus_(bus), storage_(storage) {}

// ── Legacy lifecycle ──

bool StrategyEngine::activate(uint64_t strategy_id) {
    auto* entry = registry_.find(strategy_id);
    if (!entry) return false;
    if (active_.count(strategy_id)) return false;

    // Load IR graph (with exception guard for missing/corrupt files)
    ir::StrategyGraph graph;
    try {
        graph = ir::StrategyGraph::load_from_file(entry->graph_path);
    } catch (const std::exception& e) {
        std::cerr << "[StrategyEngine] Failed to load graph '" << entry->graph_path
                  << "': " << e.what() << "\n";
        return false;
    }

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

// ── Real-time live strategy management ──

std::optional<SignalResult>
StrategyEngine::start_live(uint64_t strategy_id,
                            const ir::StrategyGraph& graph) {
    auto* entry = registry_.find(strategy_id);
    if (!entry) {
        default_logger().error("start_live: strategy not found in registry");
        return std::nullopt;
    }

    // Check if already running
    if (live_runners_.count(strategy_id)) {
        default_logger().info("start_live: already running, returning latest signal");
        return live_runners_[strategy_id]->latest_signal();
    }

    // Create and store the runner
    auto runner = std::make_unique<StrategyRunner>(
        strategy_id, graph, storage_, bus_);

    // Launch the coroutine on the global executor (fire-and-forget)
    auto* executor = network::GlobalExecutor::instance().executor();
    if (executor) {
        default_logger().info("start_live: launching runner on global executor");
        (void)folly::coro::co_withExecutor(executor, runner->run()).start();
    } else {
        default_logger().warn("start_live: global executor not available, runner will not start");
    }

    live_runners_[strategy_id] = std::move(runner);
    default_logger().info("start_live: strategy started successfully");

    // Return a valid initial signal (side=0 = hold) to indicate successful startup.
    // latest_signal() is not meaningful yet because no kline events have been
    // processed by the runner's coroutine.
    SignalResult init;
    init.timestamp = 0;
    init.symbol = "";
    init.price = 0.0;
    init.side = 0;       // hold
    init.quantity = 0;
    init.confidence = 0.0;
    return init;
}

void StrategyEngine::stop_live(uint64_t strategy_id) {
    auto it = live_runners_.find(strategy_id);
    if (it == live_runners_.end()) return;

    it->second->stop();
    live_runners_.erase(it);
}

LiveStatus StrategyEngine::live_status(uint64_t strategy_id) const {
    auto it = live_runners_.find(strategy_id);
    if (it == live_runners_.end()) {
        return LiveStatus{false, std::nullopt, {}};
    }

    LiveStatus status;
    status.is_running = it->second->running();
    status.latest_signal = it->second->latest_signal();
    status.factors = it->second->latest_factors();
    return status;
}

std::vector<uint64_t> StrategyEngine::live_strategies() const {
    std::vector<uint64_t> ids;
    ids.reserve(live_runners_.size());
    for (const auto& [id, _] : live_runners_) {
        ids.push_back(id);
    }
    return ids;
}

// ── Registry access ──

StrategyRegistry& StrategyEngine::registry() noexcept { return registry_; }
const StrategyRegistry& StrategyEngine::registry() const noexcept { return registry_; }

}  // namespace quant::strategy
