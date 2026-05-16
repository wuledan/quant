// py_factor.cc — Python bindings for factor engine
#include "cpp/quant/pybind/py_factor.h"

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/factor/built_in_factors.h"
#include "cpp/quant/factor/factor_computer.h"
#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_registry.h"

namespace py = pybind11;
using namespace quant::factor;

namespace quant::pybind {

void bind_factor(py::module_& m) {
    // ── FactorMeta ──
    py::class_<FactorMeta>(m, "FactorMeta")
        .def(py::init<>())
        .def_readwrite("name", &FactorMeta::name)
        .def_readwrite("description", &FactorMeta::description)
        .def_readwrite("inputs", &FactorMeta::inputs)
        .def_readwrite("outputs", &FactorMeta::outputs)
        .def_readwrite("version", &FactorMeta::version)
        .def_readwrite("created_at", &FactorMeta::created_at)
        .def("__repr__", [](const FactorMeta& fm) {
            return "<FactorMeta name='" + fm.name + "' v" + std::to_string(fm.version) + ">";
        });

    // ── ComputeResult ──
    py::class_<ComputeResult>(m, "ComputeResult")
        .def(py::init<>())
        .def_readwrite("success", &ComputeResult::success)
        .def_readwrite("error_msg", &ComputeResult::error_msg)
        .def_readwrite("outputs", &ComputeResult::outputs)
        .def_readwrite("compute_time_ns", &ComputeResult::compute_time_ns)
        .def("__bool__", [](const ComputeResult& r) { return r.success; });

    // ── DAGValidationResult ──
    py::class_<DAGValidationResult>(m, "DAGValidationResult")
        .def(py::init<>())
        .def_readwrite("valid", &DAGValidationResult::valid)
        .def_readwrite("message", &DAGValidationResult::message)
        .def_readwrite("cycle_path", &DAGValidationResult::cycle_path);

    // ── FactorRegistry ──
    py::class_<FactorRegistry>(m, "FactorRegistry")
        .def(py::init<>())
        .def("register_factor",
             [](FactorRegistry& reg, FactorMeta meta, py::function fn) -> FactorId {
                 // Wrap Python callable into C++ FactorComputeFn
                 return reg.register_factor(std::move(meta), fn.cast<FactorComputeFn>());
             },
             py::arg("meta"), py::arg("compute_fn"))
        .def("unregister_factor", &FactorRegistry::unregister_factor, py::arg("rule_id"))
        .def("get_meta_by_name",
             [](const FactorRegistry& reg, const std::string& name) -> const FactorMeta* {
                 return reg.get_meta(name);
             },
             py::arg("name"), py::return_value_policy::reference)
        .def("get_meta_by_id",
             [](const FactorRegistry& reg, FactorId id) -> const FactorMeta* {
                 return reg.get_meta(id);
             },
             py::arg("id"), py::return_value_policy::reference)
        .def("find_id", &FactorRegistry::find_id, py::arg("name"))
        .def("list_factors", &FactorRegistry::list_factors)
        .def("size", &FactorRegistry::size)
        .def("has_factor_by_name",
             [](const FactorRegistry& reg, const std::string& name) -> bool {
                 return reg.has_factor(name);
             },
             py::arg("name"))
        .def("has_factor_by_id",
             [](const FactorRegistry& reg, FactorId id) -> bool {
                 return reg.has_factor(id);
             },
             py::arg("id"));

    // ── FactorDAG ──
    py::class_<FactorDAG>(m, "FactorDAG")
        .def(py::init<const FactorRegistry*>(), py::arg("registry"))
        .def("build", &FactorDAG::build)
        .def("validate", &FactorDAG::validate)
        .def("topological_sort", &FactorDAG::topological_sort)
        .def("parallel_levels", &FactorDAG::parallel_levels)
        .def("get_dependencies", &FactorDAG::get_dependencies, py::arg("id"))
        .def("get_dependents", &FactorDAG::get_dependents, py::arg("id"))
        .def("clear", &FactorDAG::clear)
        .def_property_readonly("is_built", &FactorDAG::is_built);

    // ── FactorComputer ──
    py::class_<FactorComputer>(m, "FactorComputer")
        .def(py::init<std::unique_ptr<FactorRegistry>, std::unique_ptr<FactorDAG>>(),
             py::arg("registry"), py::arg("dag"))
        .def("compute_all", &FactorComputer::compute_all, py::arg("input_data"))
        .def("compute_factor", &FactorComputer::compute_factor,
             py::arg("factor_name"), py::arg("input_data"))
        .def("increment", &FactorComputer::increment,
             py::arg("changed_input"), py::arg("new_data"))
        .def("get_cached",
             [](const FactorComputer& fc, const std::string& name) -> py::object {
                 auto* v = fc.get_cached(name);
                 if (!v) return py::none();
                 return py::array_t<double>(v->size(), v->data());
             },
             py::arg("factor_name"))
        .def("invalidate", &FactorComputer::invalidate, py::arg("factor_name"))
        .def("clear_cache", &FactorComputer::clear_cache);

    // ── BuiltInFactors ──
    py::class_<BuiltInFactors>(m, "BuiltInFactors")
        .def_static("register_all", &BuiltInFactors::register_all)
        .def_static("register_ma", &BuiltInFactors::register_ma)
        .def_static("register_ema", &BuiltInFactors::register_ema)
        .def_static("register_rsi", &BuiltInFactors::register_rsi)
        .def_static("register_macd", &BuiltInFactors::register_macd)
        .def_static("register_boll", &BuiltInFactors::register_boll)
        .def_static("ma",
                    [](py::array_t<double> values, int period) -> py::array_t<double> {
                        auto buf = values.request();
                        auto* ptr = static_cast<const double*>(buf.ptr);
                        std::vector<double> v(ptr, ptr + buf.size);
                        auto result = BuiltInFactors::ma(v, period);
                        return py::array_t<double>(result.size(), result.data());
                    })
        .def_static("ema",
                    [](py::array_t<double> values, int period) -> py::array_t<double> {
                        auto buf = values.request();
                        auto* ptr = static_cast<const double*>(buf.ptr);
                        std::vector<double> v(ptr, ptr + buf.size);
                        auto result = BuiltInFactors::ema(v, period);
                        return py::array_t<double>(result.size(), result.data());
                    })
        .def_static("rsi",
                    [](py::array_t<double> values, int period) -> py::array_t<double> {
                        auto buf = values.request();
                        auto* ptr = static_cast<const double*>(buf.ptr);
                        std::vector<double> v(ptr, ptr + buf.size);
                        auto result = BuiltInFactors::rsi(v, period);
                        return py::array_t<double>(result.size(), result.data());
                    });
}

}  // namespace quant::pybind