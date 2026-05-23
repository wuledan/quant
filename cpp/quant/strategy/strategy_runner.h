// strategy_runner.h — Real-time strategy execution
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/coro/UnboundedQueue.h>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/event/events/trade_signal_event.h"
#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"
#include "cpp/quant/infra/affinity_mutex.h"
#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/ir/ir_graph.h"

namespace quant::storage { class StorageEngine; }
namespace quant::event { class EventBus; }

namespace quant::strategy {

struct SignalResult {
    int64_t timestamp;
    std::string symbol;
    double price;
    int side;           // 1=buy, -1=sell, 0=hold
    int quantity;       // suggested trade quantity
    double confidence;  // signal strength
    std::unordered_map<std::string, double> factor_values; // factor_name -> current value
};

// ── Copyable kline data used to bridge EventBus -> coroutine queue ──
struct KlineData {
    std::string symbol;
    event::DataType kline_type;
    event::KlineRow kline;
};

class StrategyRunner {
public:
    StrategyRunner(uint64_t strategy_id,
                   const ir::StrategyGraph& graph,
                   storage::StorageEngine& storage,
                   event::EventBus& bus);

    ~StrategyRunner();

    StrategyRunner(const StrategyRunner&) = delete;
    StrategyRunner& operator=(const StrategyRunner&) = delete;

    // Coroutine: consume kline events, run DAG, publish signals
    infra::CoTask<void> run();
    void stop();
    bool running() const noexcept;

    // Get latest signal and factor snapshot
    std::optional<SignalResult> latest_signal() const;
    std::unordered_map<std::string, double> latest_factors() const;

    uint64_t strategy_id() const noexcept { return strategy_id_; }

private:
    infra::CoTask<void> on_kline(KlineData data);
    SignalResult compute_signal(const event::KlineRow& kline,
                                const std::string& symbol);

    // Build input data from sliding windows
    std::unordered_map<std::string, std::vector<double>> build_input_data() const;

    // ── Kline subscriber: bridges synchronous EventBus -> coroutine queue ──
    class KlineSubscriber : public event::IEventSubscriber {
    public:
        using Queue = folly::coro::UnboundedQueue<KlineData, true, true>;

        explicit KlineSubscriber(Queue& queue) : queue_(queue) {}
        void on_event(const event::Event& event) override;

    private:
        Queue& queue_;
    };

    uint64_t strategy_id_;
    ir::StrategyGraph graph_;
    storage::StorageEngine& storage_;
    event::EventBus& bus_;
    std::unique_ptr<factor::FactorDAG> dag_;
    std::unique_ptr<factor::FactorRegistry> registry_;

    // Subscriber queue for bridging EventBus -> coroutine
    using KlineQueue = folly::coro::UnboundedQueue<KlineData, true, true>;
    KlineQueue kline_queue_;
    event::SubscriptionId kline_sub_id_{0};

    std::atomic<bool> stopped_{false};
    std::atomic<bool> running_{false};

    // Sliding windows for factor computation
    std::vector<double> close_prices_;
    std::vector<double> open_prices_;
    std::vector<double> high_prices_;
    std::vector<double> low_prices_;
    std::vector<double> volumes_;
    std::vector<int64_t> timestamps_;

    // Topological order (computed once from DAG)
    std::vector<factor::FactorId> topo_order_;

    // Cached factor values: factor_name -> full vector
    std::unordered_map<std::string, std::vector<double>> factor_values_;

    // Edge mappings from IR graph (for input port resolution)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> node_input_edges_;
    std::unordered_map<std::string, std::string> node_output_port_;

    // Latest state (protected by runner_mutex_)
    mutable infra::AffinityMutex runner_mutex_;
    std::optional<SignalResult> latest_signal_;
    std::unordered_map<std::string, double> latest_factors_;
};

}  // namespace quant::strategy
