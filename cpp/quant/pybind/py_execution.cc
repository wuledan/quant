// py_execution.cc — Python bindings for execution engine
#include "cpp/quant/pybind/py_execution.h"

#include <pybind11/stl.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cpp/quant/execution/algorithmic_trader.h"
#include "cpp/quant/execution/broker_interface.h"
#include "cpp/quant/execution/order.h"
#include "cpp/quant/execution/order_manager.h"
#include "cpp/quant/execution/order_state_machine.h"

namespace py = pybind11;
using namespace quant::execution;

namespace quant::pybind {

void bind_execution(py::module_& m) {
    // ── OrderSide ──
    py::enum_<OrderSide>(m, "OrderSide")
        .value("BUY", OrderSide::kBuy)
        .value("SELL", OrderSide::kSell)
        .export_values();

    // ── OrderType ──
    py::enum_<OrderType>(m, "OrderType")
        .value("MARKET", OrderType::kMarket)
        .value("LIMIT", OrderType::kLimit)
        .value("STOP", OrderType::kStop)
        .value("STOP_LIMIT", OrderType::kStopLimit)
        .export_values();

    // ── TimeInForce ──
    py::enum_<TimeInForce>(m, "TimeInForce")
        .value("DAY", TimeInForce::kDay)
        .value("IOC", TimeInForce::kIOC)
        .value("FOK", TimeInForce::kFOK)
        .value("GTD", TimeInForce::kGTD)
        .value("GTC", TimeInForce::kGTC)
        .export_values();

    // ── OrderStatus ──
    py::enum_<OrderStatus>(m, "OrderStatus")
        .value("PENDING_NEW", OrderStatus::kPendingNew)
        .value("NEW", OrderStatus::kNew)
        .value("PARTIAL_FILLED", OrderStatus::kPartialFilled)
        .value("FILLED", OrderStatus::kFilled)
        .value("CANCELLED", OrderStatus::kCancelled)
        .value("PENDING_CANCEL", OrderStatus::kPendingCancel)
        .value("REJECTED", OrderStatus::kRejected)
        .value("EXPIRED", OrderStatus::kExpired)
        .value("SUSPENDED", OrderStatus::kSuspended)
        .export_values();

    // ── Order struct ──
    py::class_<Order>(m, "Order")
        .def(py::init<>())
        .def_readwrite("order_id", &Order::order_id)
        .def_readwrite("client_order_id", &Order::client_order_id)
        .def_readwrite("symbol", &Order::symbol)
        .def_readwrite("side", &Order::side)
        .def_readwrite("type", &Order::type)
        .def_readwrite("time_in_force", &Order::time_in_force)
        .def_readwrite("status", &Order::status)
        .def_readwrite("price", &Order::price)
        .def_readwrite("stop_price", &Order::stop_price)
        .def_readwrite("quantity", &Order::quantity)
        .def_readwrite("filled_quantity", &Order::filled_quantity)
        .def_readwrite("filled_amount", &Order::filled_amount)
        .def_readwrite("avg_fill_price", &Order::avg_fill_price)
        .def_readwrite("created_at_ns", &Order::created_at_ns)
        .def_readwrite("updated_at_ns", &Order::updated_at_ns)
        .def_readwrite("broker_order_id", &Order::broker_order_id)
        .def_readwrite("reject_reason", &Order::reject_reason)
        .def("__repr__", [](const Order& o) {
            return "<Order id=" + std::to_string(o.order_id)
                   + " symbol=" + o.symbol
                   + " side=" + std::string(to_string(o.side))
                   + " status=" + std::string(to_string(o.status))
                   + " qty=" + std::to_string(o.quantity) + ">";
        });

    // ── OrderRequest ──
    py::class_<OrderRequest>(m, "OrderRequest")
        .def(py::init<>())
        .def_readwrite("client_order_id", &OrderRequest::client_order_id)
        .def_readwrite("symbol", &OrderRequest::symbol)
        .def_readwrite("side", &OrderRequest::side)
        .def_readwrite("type", &OrderRequest::type)
        .def_readwrite("tif", &OrderRequest::tif)
        .def_readwrite("price", &OrderRequest::price)
        .def_readwrite("stop_price", &OrderRequest::stop_price)
        .def_readwrite("quantity", &OrderRequest::quantity);

    // ── FillReport ──
    py::class_<FillReport>(m, "FillReport")
        .def(py::init<>())
        .def_readwrite("order_id", &FillReport::order_id)
        .def_readwrite("fill_quantity", &FillReport::fill_quantity)
        .def_readwrite("fill_price", &FillReport::fill_price)
        .def_readwrite("fill_time_ns", &FillReport::fill_time_ns);

    // ── OrderStateMachine (static methods) ──
    py::class_<OrderStateMachine>(m, "OrderStateMachine")
        .def_static("is_valid_transition", &OrderStateMachine::is_valid_transition)
        .def_static("apply_transition",
                    [](OrderStatus from, OrderStatus to) -> bool {
                        auto result = OrderStateMachine::apply_transition(from, to);
                        return result.ok();
                    })
        .def_static("is_terminal", &OrderStateMachine::is_terminal)
        .def_static("is_active", &OrderStateMachine::is_active)
        .def_static("can_cancel", &OrderStateMachine::can_cancel)
        .def_static("can_modify", &OrderStateMachine::can_modify);

    // ── OrderManager ──
    py::class_<OrderManager>(m, "OrderManager")
        .def(py::init<>())
        .def("create_order",
             [](OrderManager& om, const OrderRequest& req) -> py::object {
                 auto result = om.create_order(req);
                 if (result.ok()) {
                     return py::cast(result.value());
                 }
                 return py::none();
             },
             py::arg("req"))
        .def("cancel_order",
             [](OrderManager& om, OrderId order_id) -> bool {
                 auto result = om.cancel_order(order_id);
                 return result.ok();
             },
             py::arg("order_id"))
        .def("on_order_accepted", &OrderManager::on_order_accepted,
             py::arg("order_id"), py::arg("broker_order_id"))
        .def("on_order_rejected", &OrderManager::on_order_rejected,
             py::arg("order_id"), py::arg("reason"))
        .def("on_order_fill", &OrderManager::on_order_fill,
             py::arg("order_id"), py::arg("fill_qty"), py::arg("fill_price"))
        .def("on_order_cancelled", &OrderManager::on_order_cancelled, py::arg("order_id"))
        .def("on_order_expired", &OrderManager::on_order_expired, py::arg("order_id"))
        .def("on_order_suspended", &OrderManager::on_order_suspended, py::arg("order_id"))
        .def("find_order", &OrderManager::find_order, py::arg("order_id"),
             py::return_value_policy::reference)
        .def("find_orders_by_symbol",
             [](OrderManager& om, const std::string& symbol) -> py::list {
                 auto orders = om.find_orders_by_symbol(symbol);
                 py::list result;
                 for (auto* order : orders) {
                     result.append(py::cast(order, py::return_value_policy::reference));
                 }
                 return result;
             },
             py::arg("symbol"))
        .def("find_orders_by_status",
             [](OrderManager& om, OrderStatus status) -> py::list {
                 auto orders = om.find_orders_by_status(status);
                 py::list result;
                 for (auto* order : orders) {
                     result.append(py::cast(order, py::return_value_policy::reference));
                 }
                 return result;
             },
             py::arg("status"))
        .def("all_orders",
             [](OrderManager& om) -> py::list {
                 auto orders = om.all_orders();
                 py::list result;
                 for (auto* order : orders) {
                     result.append(py::cast(order, py::return_value_policy::reference));
                 }
                 return result;
             })
        .def_property_readonly("total_order_count", &OrderManager::total_order_count)
        .def_property_readonly("active_order_count", &OrderManager::active_order_count);

    // ── ConnectionStatus ──
    py::enum_<ConnectionStatus>(m, "ConnectionStatus")
        .value("DISCONNECTED", ConnectionStatus::kDisconnected)
        .value("CONNECTING", ConnectionStatus::kConnecting)
        .value("CONNECTED", ConnectionStatus::kConnected)
        .value("AUTHENTICATING", ConnectionStatus::kAuthenticating)
        .value("AUTHENTICATED", ConnectionStatus::kAuthenticated)
        .value("ERROR", ConnectionStatus::kError)
        .export_values();

    // ── BrokerConfig ──
    py::class_<BrokerConfig>(m, "BrokerConfig")
        .def(py::init<>())
        .def_readwrite("broker_id", &BrokerConfig::broker_id)
        .def_readwrite("endpoint", &BrokerConfig::endpoint)
        .def_readwrite("api_key", &BrokerConfig::api_key)
        .def_readwrite("api_secret", &BrokerConfig::api_secret)
        .def_readwrite("timeout_ms", &BrokerConfig::timeout_ms)
        .def_readwrite("auto_reconnect", &BrokerConfig::auto_reconnect)
        .def_readwrite("reconnect_interval_ms", &BrokerConfig::reconnect_interval_ms);

    // ── IBroker abstract interface (use MockBroker for testing) ──
    // Only expose the concrete MockBroker to Python for now
    py::class_<MockBroker, std::shared_ptr<MockBroker>>(m, "MockBroker")
        .def(py::init<BrokerConfig>(), py::arg("config") = BrokerConfig())
        .def("connect", &MockBroker::connect)
        .def("disconnect", &MockBroker::disconnect)
        .def_property_readonly("status", &MockBroker::status)
        .def("authenticate", &MockBroker::authenticate)
        .def("submit_order",
             [](MockBroker& b, const Order& order) { return b.submit_order(order); })
        .def("cancel_order", &MockBroker::cancel_order)
        .def("broker_id", &MockBroker::broker_id)
        .def_property_readonly("submitted", &MockBroker::submitted)
        .def_property_readonly("cancelled", &MockBroker::cancelled);

    // ── AlgoOrderConfig ──
    py::class_<AlgoOrderConfig>(m, "AlgoOrderConfig")
        .def(py::init<>())
        .def_readwrite("side", &AlgoOrderConfig::side)
        .def_readwrite("symbol", &AlgoOrderConfig::symbol)
        .def_readwrite("total_quantity", &AlgoOrderConfig::total_quantity)
        .def_readwrite("limit_price", &AlgoOrderConfig::limit_price)
        .def_readwrite("start_time_ns", &AlgoOrderConfig::start_time_ns)
        .def_readwrite("end_time_ns", &AlgoOrderConfig::end_time_ns);

    // ── AlgoTrader Stats ──
    py::class_<AlgorithmicTrader::Stats>(m, "AlgoTraderStats")
        .def(py::init<>())
        .def_readwrite("parent_order_id", &AlgorithmicTrader::Stats::parent_order_id)
        .def_readwrite("total_filled", &AlgorithmicTrader::Stats::total_filled)
        .def_readwrite("total_slices", &AlgorithmicTrader::Stats::total_slices)
        .def_readwrite("total_value", &AlgorithmicTrader::Stats::total_value);
}

}  // namespace quant::pybind
