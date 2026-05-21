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

    // Load kline data
    storage::TimeRange range{params.start_time, params.end_time};
    auto close_result = storage_.query_kline(params.symbol, params.kline_type,
                                              storage::DataField::kClose, range);
    auto ts_result = storage_.query_kline(params.symbol, params.kline_type,
                                           storage::DataField::kTimestamp, range);

    if (close_result.values.empty()) return {};

    size_t num_bars = close_result.values.size();

    // Load other price fields for factor computation
    auto open_result = storage_.query_kline(params.symbol, params.kline_type,
                                             storage::DataField::kOpen, range);
    auto high_result = storage_.query_kline(params.symbol, params.kline_type,
                                             storage::DataField::kHigh, range);
    auto low_result = storage_.query_kline(params.symbol, params.kline_type,
                                            storage::DataField::kLow, range);
    auto vol_result = storage_.query_kline(params.symbol, params.kline_type,
                                            storage::DataField::kVolume, range);

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

    // Running window for factor computation
    for (size_t i = 0; i < num_bars; ++i) {
        // Build input data for factors up to current bar
        std::unordered_map<std::string, std::vector<double>> input_data;
        input_data["price"] = std::vector<double>(
            close_result.values.begin(), close_result.values.begin() + i + 1);
        input_data["open"] = std::vector<double>(
            open_result.values.begin(), open_result.values.begin() + i + 1);
        input_data["high"] = std::vector<double>(
            high_result.values.begin(), high_result.values.begin() + i + 1);
        input_data["low"] = std::vector<double>(
            low_result.values.begin(), low_result.values.begin() + i + 1);
        input_data["volume"] = std::vector<double>(
            vol_result.values.begin(), vol_result.values.begin() + i + 1);

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
                for (auto dep_id : deps) {
                    auto* dep_meta = factor_registry.get_meta(dep_id);
                    if (dep_meta && factor_values.count(dep_meta->name)) {
                        factor_input[dep_meta->name] = factor_values[dep_meta->name];
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

        double current_price = close_result.values[i];
        int64_t ts = (i < ts_result.values.size())
                         ? static_cast<int64_t>(ts_result.values[i])
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
                    }
                }
            }
        }

        portfolio.update_market_value(params.symbol, current_price);
        portfolio.record_nav(ts);
    }

    // Calculate metrics
    BacktestResult result;
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
