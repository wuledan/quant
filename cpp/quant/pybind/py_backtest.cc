// py_backtest.cc — Python bindings for backtest + strategy registry
//
// Exposes: BacktestRunner, BacktestParams, BacktestResult,
//          SimulatedBroker, SimulatedBrokerConfig, SimulatedFill,
//          Portfolio, Position, StrategyRegistry, StrategyEntry,
//          StrategyStatus, StrategyEngine

#include "cpp/quant/pybind/py_backtest.h"

#include <pybind11/stl.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/backtest/backtest_runner.h"
#include "cpp/quant/backtest/simulated_broker.h"
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/execution/order.h"
#include "cpp/quant/ir/ir_graph.h"
#include "cpp/quant/portfolio/portfolio.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/strategy/strategy_engine.h"
#include "cpp/quant/strategy/strategy_registry.h"

namespace py = pybind11;

namespace quant::pybind {

// ── Helpers for BacktestResult.nav_curve (vector<pair<int64_t,double>>) ──
static py::list nav_curve_to_list(const std::vector<std::pair<int64_t, double>>& curve) {
    py::list result;
    for (auto& [ts, val] : curve) {
        result.append(py::make_tuple(ts, val));
    }
    return result;
}

void bind_backtest(py::module_& m) {
    // ── EventBus::Options ──
    py::class_<event::EventBus::Options>(m, "EventBusOptions")
        .def(py::init<>())
        .def_readwrite("subscriber_shard_count", &event::EventBus::Options::subscriber_shard_count)
        .def_readwrite("history_buffer_size", &event::EventBus::Options::history_buffer_size)
        .def_readwrite("enable_profiling", &event::EventBus::Options::enable_profiling);

    // ── EventBus ──
    // Required for BacktestRunner and StrategyEngine construction.
    // Only expose lifecycle methods; subscribe/publish are C++ only.
    py::class_<event::EventBus>(m, "EventBus")
        .def(py::init<event::EventBus::Options>(),
             py::arg("opts") = event::EventBus::Options())
        .def("start", &event::EventBus::start,
             "Start the EventBus worker threads")
        .def("stop", &event::EventBus::stop,
             "Stop the EventBus")
        .def("stats", &event::EventBus::stats)
        .def_static("default_options", &event::EventBus::default_options);

    // ── EventBus::Stats ──
    py::class_<event::EventBus::Stats>(m, "EventBusStats")
        .def(py::init<>())
        .def_readonly("total_published", &event::EventBus::Stats::total_published)
        .def_readonly("total_delivered", &event::EventBus::Stats::total_delivered)
        .def_readonly("avg_publish_latency_ns", &event::EventBus::Stats::avg_publish_latency_ns)
        .def_readonly("queue_depth", &event::EventBus::Stats::queue_depth)
        .def("__repr__", [](const event::EventBus::Stats& s) {
            return "<EventBusStats published=" + std::to_string(s.total_published)
                   + " delivered=" + std::to_string(s.total_delivered) + ">";
        });

    // ── BacktestParams ──
    py::class_<backtest::BacktestParams>(m, "BacktestParams")
        .def(py::init<>())
        .def_readwrite("initial_cash", &backtest::BacktestParams::initial_cash)
        .def_readwrite("start_time", &backtest::BacktestParams::start_time)
        .def_readwrite("end_time", &backtest::BacktestParams::end_time)
        .def_readwrite("symbol", &backtest::BacktestParams::symbol)
        .def_property("kline_type",
            [](const backtest::BacktestParams& p) -> int {
                return static_cast<int>(p.kline_type);
            },
            [](backtest::BacktestParams& p, int v) {
                p.kline_type = static_cast<quant::event::DataType>(v);
            })
        .def("__repr__", [](const backtest::BacktestParams& p) {
            return "<BacktestParams symbol=" + p.symbol
                   + " cash=" + std::to_string(p.initial_cash) + ">";
        });

    // ── BacktestResult ──
    py::class_<backtest::BacktestResult>(m, "BacktestResult")
        .def(py::init<>())
        .def_readonly("total_return", &backtest::BacktestResult::total_return)
        .def_readonly("annual_return", &backtest::BacktestResult::annual_return)
        .def_readonly("max_drawdown", &backtest::BacktestResult::max_drawdown)
        .def_readonly("sharpe_ratio", &backtest::BacktestResult::sharpe_ratio)
        .def_readonly("total_trades", &backtest::BacktestResult::total_trades)
        .def_property_readonly("nav_curve",
            [](const backtest::BacktestResult& r) -> py::list {
                return nav_curve_to_list(r.nav_curve);
            })
        .def("__repr__", [](const backtest::BacktestResult& r) {
            return "<BacktestResult return=" + std::to_string(r.total_return)
                   + " sharpe=" + std::to_string(r.sharpe_ratio)
                   + " max_dd=" + std::to_string(r.max_drawdown)
                   + " trades=" + std::to_string(r.total_trades) + ">";
        });

    // ── BacktestRunner ──
    // BacktestRunner takes StorageEngine& and EventBus& references.
    // Python must keep those objects alive — we use keep_alive.
    py::class_<backtest::BacktestRunner>(m, "BacktestRunner")
        .def(py::init<storage::StorageEngine&, event::EventBus&>(),
             py::arg("storage"), py::arg("bus"),
             py::keep_alive<1, 2>(), py::keep_alive<1, 3>())
        .def("run",
             static_cast<backtest::BacktestResult (backtest::BacktestRunner::*)(
                 const std::string&, const backtest::BacktestParams&)>(
                 &backtest::BacktestRunner::run),
             py::arg("graph_path"), py::arg("params"),
             "Run backtest from a graph file path")
        .def("run_from_graph",
             [](backtest::BacktestRunner& runner,
                const ir::StrategyGraph& graph,
                const backtest::BacktestParams& params) -> backtest::BacktestResult {
                 return runner.run(graph, params);
             },
             py::arg("graph"), py::arg("params"),
             "Run backtest from a StrategyGraph object");

    // ── SimulatedFill ──
    py::class_<backtest::SimulatedFill>(m, "SimulatedFill")
        .def(py::init<>())
        .def_readonly("symbol", &backtest::SimulatedFill::symbol)
        .def_readonly("side", &backtest::SimulatedFill::side)
        .def_readonly("quantity", &backtest::SimulatedFill::quantity)
        .def_readonly("price", &backtest::SimulatedFill::price)
        .def_readonly("commission", &backtest::SimulatedFill::commission)
        .def_readonly("timestamp", &backtest::SimulatedFill::timestamp)
        .def("__repr__", [](const backtest::SimulatedFill& f) {
            return "<SimulatedFill symbol=" + f.symbol
                   + " qty=" + std::to_string(f.quantity)
                   + " price=" + std::to_string(f.price) + ">";
        });

    // ── SimulatedBrokerConfig ──
    py::class_<backtest::SimulatedBrokerConfig>(m, "SimulatedBrokerConfig")
        .def(py::init<>())
        .def_readwrite("commission_rate", &backtest::SimulatedBrokerConfig::commission_rate)
        .def_readwrite("min_commission", &backtest::SimulatedBrokerConfig::min_commission)
        .def_readwrite("slippage_bps", &backtest::SimulatedBrokerConfig::slippage_bps)
        .def_readwrite("enable_slippage", &backtest::SimulatedBrokerConfig::enable_slippage)
        .def("__repr__", [](const backtest::SimulatedBrokerConfig& c) {
            return "<SimulatedBrokerConfig comm_rate=" + std::to_string(c.commission_rate)
                   + " slippage_bps=" + std::to_string(c.slippage_bps) + ">";
        });

    // ── SimulatedBroker ──
    py::class_<backtest::SimulatedBroker>(m, "SimulatedBroker")
        .def(py::init<backtest::SimulatedBrokerConfig>(),
             py::arg("config") = backtest::SimulatedBrokerConfig())
        .def("execute", &backtest::SimulatedBroker::execute,
             py::arg("symbol"), py::arg("side"),
             py::arg("quantity"), py::arg("market_price"),
             py::arg("timestamp"))
        .def_property_readonly("config", &backtest::SimulatedBroker::config,
                               py::return_value_policy::reference_internal);

    // ── Position ──
    py::class_<portfolio::Position>(m, "Position")
        .def(py::init<>())
        .def_readonly("symbol", &portfolio::Position::symbol)
        .def_readonly("quantity", &portfolio::Position::quantity)
        .def_readonly("avg_cost", &portfolio::Position::avg_cost)
        .def_readonly("market_value", &portfolio::Position::market_value)
        .def("__repr__", [](const portfolio::Position& p) {
            return "<Position symbol=" + p.symbol
                   + " qty=" + std::to_string(p.quantity)
                   + " cost=" + std::to_string(p.avg_cost) + ">";
        });

    // ── Portfolio ──
    py::class_<portfolio::Portfolio>(m, "Portfolio")
        .def(py::init<double>(), py::arg("initial_cash"))
        .def("on_fill", &portfolio::Portfolio::on_fill,
             py::arg("symbol"), py::arg("side"),
             py::arg("quantity"), py::arg("price"), py::arg("commission"))
        .def("update_market_value", &portfolio::Portfolio::update_market_value,
             py::arg("symbol"), py::arg("current_price"))
        .def_property_readonly("cash", &portfolio::Portfolio::cash)
        .def_property_readonly("total_value", &portfolio::Portfolio::total_value)
        .def_property_readonly("position_value", &portfolio::Portfolio::position_value)
        .def_property_readonly("unrealized_pnl", &portfolio::Portfolio::unrealized_pnl)
        .def("get_position",
             [](const portfolio::Portfolio& p, const std::string& symbol) -> py::object {
                 const portfolio::Position* pos = p.get_position(symbol);
                 if (pos) return py::cast(pos, py::return_value_policy::reference);
                 return py::none();
             },
             py::arg("symbol"))
        .def_property_readonly("positions",
            [](const portfolio::Portfolio& p) -> py::dict {
                py::dict result;
                for (auto& [sym, pos] : p.positions()) {
                    result[py::cast(sym)] = py::cast(pos, py::return_value_policy::reference);
                }
                return result;
            })
        .def("record_nav", &portfolio::Portfolio::record_nav, py::arg("timestamp"))
        .def_property_readonly("nav_history",
            [](const portfolio::Portfolio& p) -> py::list {
                return nav_curve_to_list(p.nav_history());
            });

    // ── StrategyStatus ──
    py::enum_<strategy::StrategyStatus>(m, "StrategyStatus")
        .value("DRAFT", strategy::StrategyStatus::kDraft)
        .value("ACTIVE", strategy::StrategyStatus::kActive)
        .value("PAUSED", strategy::StrategyStatus::kPaused)
        .value("DELETED", strategy::StrategyStatus::kDeleted)
        .export_values();

    // ── StrategyEntry ──
    py::class_<strategy::StrategyEntry>(m, "StrategyEntry")
        .def(py::init<>())
        .def_readonly("id", &strategy::StrategyEntry::id)
        .def_readwrite("name", &strategy::StrategyEntry::name)
        .def_readwrite("graph_path", &strategy::StrategyEntry::graph_path)
        .def_readwrite("status", &strategy::StrategyEntry::status)
        .def_readwrite("params", &strategy::StrategyEntry::params)
        .def_readonly("created_at", &strategy::StrategyEntry::created_at)
        .def_readonly("updated_at", &strategy::StrategyEntry::updated_at)
        .def("__repr__", [](const strategy::StrategyEntry& e) {
            return "<StrategyEntry id=" + std::to_string(e.id)
                   + " name=" + e.name + ">";
        });

    // ── StrategyRegistry ──
    py::class_<strategy::StrategyRegistry>(m, "StrategyRegistry")
        .def(py::init<>())
        .def("register_strategy", &strategy::StrategyRegistry::register_strategy,
             py::arg("name"), py::arg("graph_path"),
             py::arg("params") = std::unordered_map<std::string, double>{},
             "Register a new strategy, returns its id")
        .def("update_graph_path", &strategy::StrategyRegistry::update_graph_path,
             py::arg("id"), py::arg("graph_path"))
        .def("update_params", &strategy::StrategyRegistry::update_params,
             py::arg("id"), py::arg("params"))
        .def("update_status", &strategy::StrategyRegistry::update_status,
             py::arg("id"), py::arg("status"))
        .def("find",
             [](const strategy::StrategyRegistry& reg, uint64_t id) -> py::object {
                 const strategy::StrategyEntry* e = reg.find(id);
                 if (e) return py::cast(e, py::return_value_policy::reference);
                 return py::none();
             },
             py::arg("id"))
        .def("find_by_name",
             [](const strategy::StrategyRegistry& reg, const std::string& name) -> py::object {
                 const strategy::StrategyEntry* e = reg.find_by_name(name);
                 if (e) return py::cast(e, py::return_value_policy::reference);
                 return py::none();
             },
             py::arg("name"))
        .def("list_strategies", &strategy::StrategyRegistry::list_strategies)
        .def("list_by_status", &strategy::StrategyRegistry::list_by_status,
             py::arg("status"))
        .def("remove_strategy", &strategy::StrategyRegistry::remove_strategy,
             py::arg("id"))
        .def_property_readonly("size", &strategy::StrategyRegistry::size);

    // ── StrategyEngine ──
    // StrategyEngine takes EventBus& and StorageEngine& references.
    py::class_<strategy::StrategyEngine>(m, "StrategyEngine")
        .def(py::init<event::EventBus&, storage::StorageEngine&>(),
             py::arg("bus"), py::arg("storage"),
             py::keep_alive<1, 2>(), py::keep_alive<1, 3>())
        .def("activate", &strategy::StrategyEngine::activate, py::arg("strategy_id"))
        .def("pause", &strategy::StrategyEngine::pause, py::arg("strategy_id"))
        .def("resume", &strategy::StrategyEngine::resume, py::arg("strategy_id"))
        .def("deactivate", &strategy::StrategyEngine::deactivate, py::arg("strategy_id"))
        .def("is_active", &strategy::StrategyEngine::is_active, py::arg("strategy_id"))
        .def_property_readonly("registry",
            [](strategy::StrategyEngine& e) -> strategy::StrategyRegistry& {
                return e.registry();
            },
            py::return_value_policy::reference_internal);
}

}  // namespace quant::pybind
