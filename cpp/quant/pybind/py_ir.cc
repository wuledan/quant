// py_ir.cc — Python bindings for IR graph (StrategyGraph, NodeDef, EdgeDef, etc.)
#include "cpp/quant/pybind/py_ir.h"

#include <pybind11/stl.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/ir/ir_graph.h"

namespace py = pybind11;
using namespace quant::ir;

namespace quant::pybind {

void bind_ir(py::module_& m) {
    // ── TypeSpec ──
    py::class_<TypeSpec>(m, "TypeSpec")
        .def(py::init<>())
        .def(py::init<std::string, std::string>(),
             py::arg("base_type"), py::arg("inner_type"))
        .def_readwrite("base_type", &TypeSpec::base_type)
        .def_readwrite("inner_type", &TypeSpec::inner_type)
        .def("compatible", &TypeSpec::compatible, py::arg("other"))
        .def("__eq__", &TypeSpec::operator==)
        .def("__repr__", [](const TypeSpec& t) {
            return "<TypeSpec base=" + t.base_type + " inner=" + t.inner_type + ">";
        });

    // ── PortDef ──
    py::class_<PortDef>(m, "PortDef")
        .def(py::init<>())
        .def_readwrite("name", &PortDef::name)
        .def_readwrite("source", &PortDef::source)
        .def_property("type",
            [](const PortDef& p) -> py::dict {
                py::dict d;
                d["base_type"] = p.type.base_type;
                d["inner_type"] = p.type.inner_type;
                return d;
            },
            [](PortDef& p, py::dict d) {
                p.type.base_type = d["base_type"].cast<std::string>();
                p.type.inner_type = d["inner_type"].cast<std::string>();
            })
        .def("__repr__", [](const PortDef& p) {
            return "<PortDef name=" + p.name + " source=" + p.source + ">";
        });

    // ── NodeDef ──
    py::class_<NodeDef>(m, "NodeDef")
        .def(py::init<>())
        .def_readwrite("id", &NodeDef::id)
        .def_readwrite("op_type", &NodeDef::op_type)
        .def_readwrite("inputs", &NodeDef::inputs)
        .def_readwrite("outputs", &NodeDef::outputs)
        .def_readwrite("params", &NodeDef::params)
        .def("__repr__", [](const NodeDef& n) {
            return "<NodeDef id=" + n.id + " op=" + n.op_type + ">";
        });

    // ── EdgeDef ──
    py::class_<EdgeDef>(m, "EdgeDef")
        .def(py::init<>())
        .def(py::init<std::string, std::string, std::string, std::string>(),
             py::arg("from_node"), py::arg("from_port"),
             py::arg("to_node"), py::arg("to_port"))
        .def_readwrite("from_node", &EdgeDef::from_node)
        .def_readwrite("from_port", &EdgeDef::from_port)
        .def_readwrite("to_node", &EdgeDef::to_node)
        .def_readwrite("to_port", &EdgeDef::to_port)
        .def("__repr__", [](const EdgeDef& e) {
            return "<EdgeDef " + e.from_node + "." + e.from_port
                   + " -> " + e.to_node + "." + e.to_port + ">";
        });

    // ── DataBinding ──
    py::class_<DataBinding>(m, "DataBinding")
        .def(py::init<>())
        .def(py::init<std::string, std::string, std::string>(),
             py::arg("data_source"), py::arg("to_node"), py::arg("to_port"))
        .def_readwrite("data_source", &DataBinding::data_source)
        .def_readwrite("to_node", &DataBinding::to_node)
        .def_readwrite("to_port", &DataBinding::to_port)
        .def("__repr__", [](const DataBinding& b) {
            return "<DataBinding " + b.data_source + " -> " + b.to_node + "." + b.to_port + ">";
        });

    // ── SignalHandler (IR struct) ──
    py::class_<SignalHandler>(m, "IRSignalHandler")
        .def(py::init<>())
        .def(py::init<std::string, std::string,
                      std::unordered_map<std::string, double>>(),
             py::arg("signal_node"), py::arg("handler_type"),
             py::arg("params") = std::unordered_map<std::string, double>{})
        .def_readwrite("signal_node", &SignalHandler::signal_node)
        .def_readwrite("handler_type", &SignalHandler::handler_type)
        .def_readwrite("params", &SignalHandler::params)
        .def("__repr__", [](const SignalHandler& h) {
            return "<IRSignalHandler node=" + h.signal_node
                   + " type=" + h.handler_type + ">";
        });

    // ── StrategyGraph ──
    py::class_<StrategyGraph>(m, "StrategyGraph")
        .def(py::init<>())
        .def_readwrite("strategy_name", &StrategyGraph::strategy_name)
        .def_readwrite("version", &StrategyGraph::version)
        .def_readwrite("nodes", &StrategyGraph::nodes)
        .def_readwrite("edges", &StrategyGraph::edges)
        .def_readwrite("data_bindings", &StrategyGraph::data_bindings)
        .def_readwrite("signal_handlers", &StrategyGraph::signal_handlers)
        .def_static("load_from_file", &StrategyGraph::load_from_file,
                    py::arg("path"),
                    "Load a StrategyGraph from a JSON file")
        .def_static("load_from_json", &StrategyGraph::load_from_json,
                    py::arg("json_str"),
                    "Load a StrategyGraph from a JSON string")
        .def("to_json", &StrategyGraph::to_json,
             "Serialize the graph to a JSON string")
        .def("write_to_file", &StrategyGraph::write_to_file,
             py::arg("path"),
             "Write the graph to a JSON file")
        .def("validate",
             [](const StrategyGraph& g) -> py::object {
                 std::string error_msg;
                 bool ok = g.validate(error_msg);
                 if (ok) {
                     return py::make_tuple(true, py::none());
                 }
                 return py::make_tuple(false, py::cast(error_msg));
             },
             "Validate the graph. Returns (True, None) or (False, error_msg)")
        .def("find_node",
             [](const StrategyGraph& g, const std::string& id) -> py::object {
                 const NodeDef* n = g.find_node(id);
                 if (n) return py::cast(n, py::return_value_policy::reference);
                 return py::none();
             },
             py::arg("id"),
             "Find a node by id, returns NodeDef or None")
        .def("nodes_by_op",
             [](const StrategyGraph& g, const std::string& op_type) -> py::list {
                 auto nodes = g.nodes_by_op(op_type);
                 py::list result;
                 for (auto* n : nodes) {
                     result.append(py::cast(n, py::return_value_policy::reference));
                 }
                 return result;
             },
             py::arg("op_type"),
             "Find all nodes with the given op_type")
        .def("__repr__", [](const StrategyGraph& g) {
            return "<StrategyGraph name=" + g.strategy_name
                   + " v=" + std::to_string(g.version)
                   + " nodes=" + std::to_string(g.nodes.size())
                   + " edges=" + std::to_string(g.edges.size()) + ">";
        });
}

}  // namespace quant::pybind
