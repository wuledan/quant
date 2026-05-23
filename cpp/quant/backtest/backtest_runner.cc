// backtest_runner.cc — Backtest runner implementation
#include "cpp/quant/backtest/backtest_runner.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "cpp/quant/backtest/simulated_broker.h"
#include "cpp/quant/execution/order_manager.h"
#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"
#include "cpp/quant/factor/op_registry.h"
#include "cpp/quant/ir/ir_graph.h"
#include "cpp/quant/portfolio/portfolio.h"
#include "cpp/quant/strategy/signal_handler.h"

namespace quant::backtest {

BacktestRunner::BacktestRunner(storage::StorageEngine& storage, event::EventBus& bus)
    : storage_(storage), bus_(bus) {}

BacktestResult BacktestRunner::run(const std::string& graph_path,
                                    const BacktestParams& params) {
    auto graph = ir::StrategyGraph::load_from_file(graph_path);
    return run(graph, params);
}

BacktestResult BacktestRunner::run(const ir::StrategyGraph& graph,
                                    const BacktestParams& params) {
    factor::OpRegistry::register_all_builtin_ops();

    factor::FactorRegistry factor_registry;
    auto dag = factor::FactorDAG::from_graph(graph, factor_registry);
    if (!dag || !dag->is_built()) {
        return {};
    }

    auto topo = dag->topological_sort();

    // Build edge mapping: (to_node_id, to_port) → (from_node_id, from_port)
    // This maps IR port names so factor inputs use the correct keys.
    // E.g., for edge fast_ma.value → signal.fast, the signal node receives
    // its dependency's output under key "fast" (the to_port), not "fast_ma" (the node name).
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        node_input_edges;  // node_id → {to_port → from_node_id}

    // Also track: from_node_id → output port name for value lookup
    std::unordered_map<std::string, std::string> node_output_port;  // node_id → from_port (typically "value")

    for (const auto& edge : graph.edges) {
        node_input_edges[edge.to_node][edge.to_port] = edge.from_node;
        node_output_port[edge.from_node] = edge.from_port;
    }

    // Load kline data via new KlineRow API
    auto klines = storage_.query_kline(params.symbol, params.kline_type,
                                        params.start_time, params.end_time);
    if (klines.empty()) return {};

    size_t num_bars = klines.size();

    // Extract per-field arrays for factor computation (convert int32×10000 → double)
    std::vector<double> close_vals, open_vals, high_vals, low_vals, vol_vals, ts_vals;
    close_vals.reserve(num_bars); open_vals.reserve(num_bars);
    high_vals.reserve(num_bars); low_vals.reserve(num_bars);
    vol_vals.reserve(num_bars); ts_vals.reserve(num_bars);

    for (const auto& k : klines) {
        ts_vals.push_back(static_cast<double>(k.timestamp));
        open_vals.push_back(static_cast<double>(k.open_price) / 10000.0);
        high_vals.push_back(static_cast<double>(k.high_price) / 10000.0);
        low_vals.push_back(static_cast<double>(k.low_price) / 10000.0);
        close_vals.push_back(static_cast<double>(k.close_price) / 10000.0);
        vol_vals.push_back(static_cast<double>(k.volume));
    }

    portfolio::Portfolio portfolio(params.initial_cash);
    SimulatedBroker broker;
    execution::OrderManager order_mgr;

    // Build signal handlers from IR
    std::vector<std::unique_ptr<strategy::ISignalHandler>> handlers;
    for (const auto& sh_def : graph.signal_handlers) {
        auto handler = strategy::SignalHandlerFactory::create(
            sh_def.handler_type, sh_def.params, order_mgr, bus_);
        if (handler) {
            handlers.push_back(std::move(handler));
        }
    }
    if (handlers.empty()) {
        // Default: create an order handler
        handlers.push_back(std::make_unique<strategy::OrderSignalHandler>(
            strategy::OrderSignalHandler::Params{}, order_mgr, bus_));
    }

    // Factor value storage: factor_name → vector of values
    std::unordered_map<std::string, std::vector<double>> factor_values;

    BacktestResult result;

    // Running window for factor computation
    for (size_t i = 0; i < num_bars; ++i) {
        // Build input data for factors up to current bar
        std::unordered_map<std::string, std::vector<double>> input_data;
        input_data["price"] = std::vector<double>(
            close_vals.begin(), close_vals.begin() + i + 1);
        input_data["open"] = std::vector<double>(
            open_vals.begin(), open_vals.begin() + i + 1);
        input_data["high"] = std::vector<double>(
            high_vals.begin(), high_vals.begin() + i + 1);
        input_data["low"] = std::vector<double>(
            low_vals.begin(), low_vals.begin() + i + 1);
        input_data["volume"] = std::vector<double>(
            vol_vals.begin(), vol_vals.begin() + i + 1);

        // Compute factors in topological order
        for (auto fid : topo) {
            auto* meta = factor_registry.get_meta(fid);
            if (!meta) continue;
            auto* fn = factor_registry.get_compute_fn(fid);
            if (!fn) continue;

            // Build factor-specific inputs from dependencies
            std::unordered_map<std::string, std::vector<double>> factor_input;
            auto deps = dag->get_dependencies(fid);
            if (deps.empty()) {
                // Leaf node: use data bindings from graph
                factor_input = input_data;
            } else {
                // Use IR edge port names as input keys.
                // E.g., if edge says fast_ma.value → signal.fast,
                // then input key is "fast" (to_port) and value is output of fast_ma.
                auto edge_it = node_input_edges.find(meta->name);
                if (edge_it != node_input_edges.end()) {
                    for (const auto& [to_port, from_node_id] : edge_it->second) {
                        auto val_it = factor_values.find(from_node_id);
                        if (val_it != factor_values.end()) {
                            factor_input[to_port] = val_it->second;
                        }
                    }
                } else {
                    // Fallback: use factor name as key
                    for (auto dep_id : deps) {
                        auto* dep_meta = factor_registry.get_meta(dep_id);
                        if (dep_meta && factor_values.count(dep_meta->name)) {
                            factor_input[dep_meta->name] = factor_values[dep_meta->name];
                        }
                    }
                }
            }

            auto output = (*fn)(factor_input);
            for (auto& [key, vals] : output) {
                factor_values[meta->name] = std::move(vals);
            }
        }

        // Get signal value from the last factor (signal node)
        double signal_value = 0.0;
        if (!topo.empty()) {
            auto* last_meta = factor_registry.get_meta(topo.back());
            if (last_meta && factor_values.count(last_meta->name)) {
                auto& sv = factor_values[last_meta->name];
                if (!sv.empty()) signal_value = sv.back();
            }
        }

        double current_price = close_vals[i];
        int64_t ts = (i < ts_vals.size())
                         ? static_cast<int64_t>(ts_vals[i])
                         : static_cast<int64_t>(i);

        // Invoke signal handlers
        for (auto& handler : handlers) {
            strategy::SignalContext ctx;
            ctx.symbol = params.symbol;
            ctx.price = current_price;
            ctx.cash = portfolio.cash();
            ctx.position = (portfolio.get_position(params.symbol))
                               ? portfolio.get_position(params.symbol)->quantity
                               : 0.0;
            ctx.timestamp = ts;
            ctx.signal_value = signal_value;

            size_t orders_before = order_mgr.total_order_count();
            handler->handle(signal_value, ctx);

            // Process any new orders
            if (order_mgr.total_order_count() > orders_before) {
                auto orders = order_mgr.all_orders();
                for (auto* order : orders) {
                    if (order->status == execution::OrderStatus::kPendingNew) {
                        auto fill = broker.execute(
                            order->symbol, order->side,
                            static_cast<double>(order->quantity),
                            current_price, ts);
                        portfolio.on_fill(fill.symbol, fill.side,
                                          fill.quantity, fill.price, fill.commission);

                        // Record trade
                        TradeEntry trade;
                        trade.timestamp = ts;
                        trade.price = fill.price;
                        trade.side = (fill.side == execution::OrderSide::kBuy) ? "buy" : "sell";
                        trade.quantity = static_cast<int64_t>(fill.quantity);
                        result.trades.push_back(std::move(trade));
                    }
                }
            }
        }

        portfolio.update_market_value(params.symbol, current_price);
        portfolio.record_nav(ts);
    }

    // Calculate metrics
    result.nav_curve = portfolio.nav_history();
    result.total_trades = static_cast<int64_t>(order_mgr.total_order_count());

    if (!result.nav_curve.empty()) {
        double final_nav = result.nav_curve.back().second;
        result.total_return = (final_nav / params.initial_cash) - 1.0;

        // Max drawdown
        double peak = result.nav_curve[0].second;
        double max_dd = 0.0;
        for (const auto& [_, nav] : result.nav_curve) {
            if (nav > peak) peak = nav;
            double dd = (peak - nav) / peak;
            if (dd > max_dd) max_dd = dd;
        }
        result.max_drawdown = max_dd;

        // Annualized return (assume 252 trading days)
        if (result.nav_curve.size() > 1) {
            double days = static_cast<double>(result.nav_curve.size() - 1);
            result.annual_return = std::pow(1.0 + result.total_return, 252.0 / days) - 1.0;
        }

        // Sharpe ratio (simplified: daily returns, risk-free = 0)
        if (result.nav_curve.size() > 2) {
            std::vector<double> daily_returns;
            daily_returns.reserve(result.nav_curve.size() - 1);
            for (size_t i = 1; i < result.nav_curve.size(); ++i) {
                daily_returns.push_back(
                    result.nav_curve[i].second / result.nav_curve[i - 1].second - 1.0);
            }
            double mean = std::accumulate(daily_returns.begin(), daily_returns.end(), 0.0)
                          / daily_returns.size();
            double sq_sum = 0.0;
            for (double r : daily_returns) {
                sq_sum += (r - mean) * (r - mean);
            }
            double stddev = std::sqrt(sq_sum / (daily_returns.size() - 1));
            result.sharpe_ratio = (stddev > 0) ? mean / stddev * std::sqrt(252.0) : 0.0;
        }
    }

    return result;
}

}  // namespace backtest
