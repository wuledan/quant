// py_risk.cc — Python bindings for risk engine
#include "cpp/quant/pybind/py_risk.h"

#include <pybind11/stl.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/risk/risk_alert_publisher.h"
#include "cpp/quant/risk/risk_engine.h"
#include "cpp/quant/risk/risk_rule.h"

namespace py = pybind11;
using namespace quant::risk;

namespace quant::pybind {

// ── Trampoline for IRiskRule to allow Python subclassing ──
class PyRiskRule : public IRiskRule {
public:
    using IRiskRule::IRiskRule;

    RuleId id() const noexcept override {
        PYBIND11_OVERRIDE_PURE(RuleId, IRiskRule, id);
    }

    std::string_view name() const noexcept override {
        PYBIND11_OVERRIDE_PURE(std::string_view, IRiskRule, name);
    }

    RiskCheckResult check(const RiskContext& ctx) const override {
        PYBIND11_OVERRIDE_PURE(RiskCheckResult, IRiskRule, check, ctx);
    }

    bool validate() const noexcept override {
        PYBIND11_OVERRIDE_PURE(bool, IRiskRule, validate);
    }

    void enable() noexcept override {
        PYBIND11_OVERRIDE(void, IRiskRule, enable);
    }

    void disable() noexcept override {
        PYBIND11_OVERRIDE(void, IRiskRule, disable);
    }

    bool is_enabled() const noexcept override {
        PYBIND11_OVERRIDE(bool, IRiskRule, is_enabled);
    }
};

void bind_risk(py::module_& m) {
    // ── RuleId ──
    py::class_<RuleId>(m, "RuleId");

    // ── RiskCheckResult ──
    py::class_<RiskCheckResult>(m, "RiskCheckResult")
        .def(py::init<>())
        .def_readwrite("approved", &RiskCheckResult::approved)
        .def_readwrite("rule_id", &RiskCheckResult::rule_id)
        .def_readwrite("rule_name", &RiskCheckResult::rule_name)
        .def_readwrite("message", &RiskCheckResult::message)
        .def_readwrite("exposure", &RiskCheckResult::exposure)
        .def_readwrite("limit", &RiskCheckResult::limit)
        .def_static("pass_result", &RiskCheckResult::pass)
        .def_static(
            "reject_result",
            [](RuleId id, std::string name, std::string msg,
               double exposure, double limit) {
                return RiskCheckResult::reject(id, std::move(name),
                                               std::move(msg), exposure, limit);
            },
            py::arg("rule_id"), py::arg("rule_name"), py::arg("message"),
            py::arg("exposure") = 0.0, py::arg("limit") = 0.0)
        .def("__bool__", [](const RiskCheckResult& r) { return r.approved; });

    // ── RiskContext ──
    py::class_<RiskContext>(m, "RiskContext")
        .def(py::init<>())
        .def_readwrite("total_equity", &RiskContext::total_equity)
        .def_readwrite("available_cash", &RiskContext::available_cash)
        .def_readwrite("total_positions", &RiskContext::total_positions)
        .def_readwrite("unrealized_pnl", &RiskContext::unrealized_pnl)
        .def_readwrite("realized_pnl", &RiskContext::realized_pnl)
        .def_readwrite("daily_pnl", &RiskContext::daily_pnl)
        .def_readwrite("max_drawdown", &RiskContext::max_drawdown)
        .def_readwrite("symbol_positions", &RiskContext::symbol_positions)
        .def_readwrite("symbol_quantities", &RiskContext::symbol_quantities)
        .def_readwrite("symbol_prices", &RiskContext::symbol_prices)
        .def_readwrite("order_symbol", &RiskContext::order_symbol)
        .def_readwrite("order_quantity", &RiskContext::order_quantity)
        .def_readwrite("order_price", &RiskContext::order_price)
        .def_readwrite("order_side", &RiskContext::order_side)
        .def_readwrite("market_volatility", &RiskContext::market_volatility);

    // ── IRiskRule (abstract, with trampoline) ──
    py::class_<IRiskRule, PyRiskRule>(m, "IRiskRule")
        .def(py::init<>())
        .def("id", &IRiskRule::id)
        .def("name", [](const IRiskRule& r) -> std::string {
            return std::string(r.name());
        })
        .def("check", &IRiskRule::check)
        .def("validate", &IRiskRule::validate)
        .def("enable", &IRiskRule::enable)
        .def("disable", &IRiskRule::disable)
        .def_property_readonly("is_enabled", &IRiskRule::is_enabled);

    // ── Built-in rules ──
    py::class_<MaxDrawdownRule, IRiskRule>(m, "MaxDrawdownRule")
        .def(py::init<double>(), py::arg("max_drawdown_pct"))
        .def_property_readonly("max_drawdown_pct", &MaxDrawdownRule::max_drawdown_pct);

    py::class_<ConcentrationRule, IRiskRule>(m, "ConcentrationRule")
        .def(py::init<double>(), py::arg("max_concentration_pct"))
        .def_property_readonly("max_concentration_pct", &ConcentrationRule::max_concentration_pct);

    py::class_<ExposureRule, IRiskRule>(m, "ExposureRule")
        .def(py::init<double>(), py::arg("max_exposure_ratio"))
        .def_property_readonly("max_exposure_ratio", &ExposureRule::max_exposure_ratio);

    py::class_<LimitRule, IRiskRule>(m, "LimitRule")
        .def(py::init<double, double>(),
             py::arg("max_order_value"), py::arg("max_total_value"))
        .def_property_readonly("max_order_value", &LimitRule::max_order_value)
        .def_property_readonly("max_total_value", &LimitRule::max_total_value);

    // ── CircuitBreakerConfig ──
    py::class_<CircuitBreakerConfig>(m, "CircuitBreakerConfig")
        .def(py::init<>())
        .def_readwrite("drawdown_threshold", &CircuitBreakerConfig::drawdown_threshold)
        .def_readwrite("loss_threshold", &CircuitBreakerConfig::loss_threshold)
        .def_readwrite("max_consecutive_rejects", &CircuitBreakerConfig::max_consecutive_rejects);

    // ── RiskEngineStats ──
    py::class_<RiskEngineStats>(m, "RiskEngineStats")
        .def(py::init<>())
        .def_readwrite("total_checks", &RiskEngineStats::total_checks)
        .def_readwrite("total_approvals", &RiskEngineStats::total_approvals)
        .def_readwrite("total_rejections", &RiskEngineStats::total_rejections)
        .def_readwrite("circuit_breaks", &RiskEngineStats::circuit_breaks);

    // ── RiskEngine::CheckResult ──
    py::class_<RiskEngine::CheckResult>(m, "RiskCheckResultSet")
        .def(py::init<>())
        .def_readwrite("approved", &RiskEngine::CheckResult::approved)
        .def_readwrite("rule_results", &RiskEngine::CheckResult::rule_results)
        .def("__bool__", [](const RiskEngine::CheckResult& r) { return r.approved; });

    // ── RiskEngine ──
    py::class_<RiskEngine>(m, "RiskEngine")
        .def(py::init<CircuitBreakerConfig>(), py::arg("cb_config") = CircuitBreakerConfig())
        .def("register_rule", &RiskEngine::register_rule, py::arg("rule"))
        .def("unregister_rule", &RiskEngine::unregister_rule, py::arg("rule_id"))
        .def("find_rule", &RiskEngine::find_rule, py::arg("rule_id"),
             py::return_value_policy::reference)
        .def("all_rules", &RiskEngine::all_rules,
             py::return_value_policy::reference)
        .def("check", &RiskEngine::check, py::arg("ctx"))
        .def_property_readonly("is_circuit_break", &RiskEngine::is_circuit_break)
        .def("reset_circuit_break", &RiskEngine::reset_circuit_break)
        .def("enable", &RiskEngine::enable)
        .def("disable", &RiskEngine::disable)
        .def_property_readonly("is_enabled", &RiskEngine::is_enabled)
        .def_property_readonly("stats", &RiskEngine::stats);

    // ── AlertSeverity ──
    py::enum_<AlertSeverity>(m, "AlertSeverity")
        .value("INFO", AlertSeverity::kInfo)
        .value("WARNING", AlertSeverity::kWarning)
        .value("ERROR", AlertSeverity::kError)
        .value("CRITICAL", AlertSeverity::kCritical)
        .export_values();

    // ── RiskAlert (data-only struct, no EventBus dependency) ──
    py::class_<RiskAlert>(m, "RiskAlert")
        .def(py::init<>())
        .def_readwrite("rule_id", &RiskAlert::rule_id)
        .def_readwrite("rule_name", &RiskAlert::rule_name)
        .def_readwrite("severity", &RiskAlert::severity)
        .def_readwrite("message", &RiskAlert::message)
        .def_readwrite("value", &RiskAlert::value)
        .def_readwrite("threshold", &RiskAlert::threshold)
        .def_readwrite("timestamp_ns", &RiskAlert::timestamp_ns);
}

}  // namespace quant::pybind