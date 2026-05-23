// strategy_runner.cc — Real-time strategy execution implementation
#include "cpp/quant/strategy/strategy_runner.h"

#include "cpp/quant/factor/op_registry.h"
#include "cpp/quant/storage/storage_engine.h"

namespace quant::strategy {

// ── KlineSubscriber::on_event — copies fields into the queue ──

void StrategyRunner::KlineSubscriber::on_event(const event::Event& event) {
    auto& ke = static_cast<const event::KlineEvent&>(event);
    queue_.enqueue(KlineData{ke.symbol, ke.kline_type, ke.kline});
}

// ── Constructor / Destructor ──

StrategyRunner::StrategyRunner(uint64_t strategy_id,
                                const ir::StrategyGraph& graph,
                                storage::StorageEngine& storage,
                                event::EventBus& bus)
    : strategy_id_(strategy_id)
    , graph_(graph)
    , storage_(storage)
    , bus_(bus) {}

StrategyRunner::~StrategyRunner() {
    stop();
}

void StrategyRunner::stop() {
    stopped_.store(true, std::memory_order_release);
    if (kline_sub_id_ != 0) {
        bus_.unsubscribe(kline_sub_id_);
        kline_sub_id_ = 0;
    }
}

bool StrategyRunner::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::optional<SignalResult> StrategyRunner::latest_signal() const {
    if (runner_mutex_.try_lock()) {
        auto result = latest_signal_;
        runner_mutex_.unlock();
        return result;
    }
    return std::nullopt;
}

std::unordered_map<std::string, double> StrategyRunner::latest_factors() const {
    if (runner_mutex_.try_lock()) {
        auto result = latest_factors_;
        runner_mutex_.unlock();
        return result;
    }
    return {};
}

// ── Build input data from sliding windows ──

std::unordered_map<std::string, std::vector<double>>
StrategyRunner::build_input_data() const {
    std::unordered_map<std::string, std::vector<double>> data;
    data["price"]  = close_prices_;
    data["open"]   = open_prices_;
    data["high"]   = high_prices_;
    data["low"]    = low_prices_;
    data["volume"] = volumes_;
    return data;
}

// ── Main run loop ──

infra::CoTask<void> StrategyRunner::run() {
    // Build DAG from IR graph
    factor::OpRegistry::register_all_builtin_ops();
    registry_ = std::make_unique<factor::FactorRegistry>();
    dag_ = factor::FactorDAG::from_graph(graph_, *registry_);
    if (!dag_ || !dag_->is_built()) {
        running_.store(false, std::memory_order_release);
        co_return;
    }

    topo_order_ = dag_->topological_sort();

    // Build edge mappings (same pattern as backtest_runner.cc)
    for (const auto& edge : graph_.edges) {
        node_input_edges_[edge.to_node][edge.to_port] = edge.from_node;
        node_output_port_[edge.from_node] = edge.from_port;
    }

    // Subscribe to KlineEvent via coroutine queue bridge
    kline_sub_id_ = bus_.subscribe(
        event::KlineEvent::kEventTypeId,
        std::make_unique<KlineSubscriber>(kline_queue_));

    running_.store(true, std::memory_order_release);

    // Main loop: dequeue kline events and process them
    while (!stopped_.load(std::memory_order_acquire)) {
        auto data = co_await kline_queue_.dequeue();
        co_await on_kline(std::move(data));
    }

    // Cleanup
    if (kline_sub_id_ != 0) {
        bus_.unsubscribe(kline_sub_id_);
        kline_sub_id_ = 0;
    }
    running_.store(false, std::memory_order_release);
}

// ── Per-kline processing ──

infra::CoTask<void> StrategyRunner::on_kline(KlineData data) {
    // Update sliding windows
    close_prices_.push_back(static_cast<double>(data.kline.close_price) / 10000.0);
    open_prices_.push_back(static_cast<double>(data.kline.open_price) / 10000.0);
    high_prices_.push_back(static_cast<double>(data.kline.high_price) / 10000.0);
    low_prices_.push_back(static_cast<double>(data.kline.low_price) / 10000.0);
    volumes_.push_back(static_cast<double>(data.kline.volume));
    timestamps_.push_back(data.kline.timestamp);

    // Compute signal
    auto result = compute_signal(data.kline, data.symbol);

    // Publish TradeSignalEvent (skip hold signals)
    if (result.side != 0) {
        auto sig_event = std::make_unique<event::TradeSignalEvent>();
        sig_event->strategy_id = std::to_string(strategy_id_);
        sig_event->symbol = result.symbol;
        sig_event->side = (result.side > 0) ? event::OrderSide::kBuy
                                            : event::OrderSide::kSell;
        sig_event->target_weight = static_cast<double>(result.quantity) / 100.0;
        sig_event->confidence = result.confidence;
        sig_event->price = result.price;
        sig_event->quantity = result.quantity;
        sig_event->factor_values = result.factor_values;
        sig_event->set_timestamp_us(result.timestamp);
        bus_.publish(std::move(sig_event));
    }

    // Update latest snapshot under mutex
    {
        auto lock = co_await runner_mutex_.co_scoped_lock();
        latest_signal_ = std::move(result);
        latest_factors_ = latest_signal_->factor_values;
    }
}

// ── Signal computation ──

SignalResult StrategyRunner::compute_signal(const event::KlineRow& kline,
                                             const std::string& symbol) {
    SignalResult result;
    result.timestamp = kline.timestamp;
    result.symbol = symbol;
    result.price = static_cast<double>(kline.close_price) / 10000.0;
    result.side = 0;
    result.quantity = 0;
    result.confidence = 0.0;

    if (topo_order_.empty()) {
        return result;
    }

    auto input_data = build_input_data();

    // Compute factors in topological order (same pattern as backtest_runner.cc)
    for (auto fid : topo_order_) {
        auto* meta = registry_->get_meta(fid);
        if (!meta) continue;
        auto* fn = registry_->get_compute_fn(fid);
        if (!fn) continue;

        std::unordered_map<std::string, std::vector<double>> factor_input;
        auto deps = dag_->get_dependencies(fid);
        if (deps.empty()) {
            factor_input = input_data;
        } else {
            auto edge_it = node_input_edges_.find(meta->name);
            if (edge_it != node_input_edges_.end()) {
                for (const auto& [to_port, from_node_id] : edge_it->second) {
                    auto val_it = factor_values_.find(from_node_id);
                    if (val_it != factor_values_.end()) {
                        factor_input[to_port] = val_it->second;
                    }
                }
            } else {
                for (auto dep_id : deps) {
                    auto* dep_meta = registry_->get_meta(dep_id);
                    if (dep_meta && factor_values_.count(dep_meta->name)) {
                        factor_input[dep_meta->name] = factor_values_[dep_meta->name];
                    }
                }
            }
        }

        auto output = (*fn)(factor_input);
        for (auto& [key, vals] : output) {
            factor_values_[meta->name] = std::move(vals);
        }
    }

    // Extract signal from last factor node
    auto* last_meta = registry_->get_meta(topo_order_.back());
    if (last_meta && factor_values_.count(last_meta->name)) {
        auto& sv = factor_values_[last_meta->name];
        if (!sv.empty()) {
            double signal_val = sv.back();
            result.confidence = std::abs(signal_val);
            result.side = (signal_val > 0.001) ? 1 : (signal_val < -0.001) ? -1 : 0;
            result.quantity = static_cast<int>(std::abs(signal_val) * 100);
        }
    }

    // Collect factor snapshots (latest value per factor)
    result.factor_values.clear();
    for (auto fid : topo_order_) {
        auto* meta = registry_->get_meta(fid);
        if (meta && factor_values_.count(meta->name)) {
            auto& vals = factor_values_[meta->name];
            if (!vals.empty()) {
                result.factor_values[meta->name] = vals.back();
            }
        }
    }

    return result;
}

}  // namespace quant::strategy
