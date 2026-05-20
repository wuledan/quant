// ir_graph.h — Strategy Graph IR data structures
//
// Defines the intermediate representation for compiled strategy graphs.
// A StrategyGraph describes the computation topology: nodes (factors/signals),
// edges (data flow), data bindings (storage → node inputs), and signal handlers.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace quant::ir {

struct TypeSpec {
    std::string base_type;    // "TimeSeries", "Signal", "Scalar"
    std::string inner_type;   // "float", "int64", "bool"

    bool operator==(const TypeSpec& o) const {
        return base_type == o.base_type && inner_type == o.inner_type;
    }
    bool compatible(const TypeSpec& o) const {
        return base_type == o.base_type && inner_type == o.inner_type;
    }
};

struct PortDef {
    std::string name;
    TypeSpec    type;
    std::string source;       // "data.close" or "node.fast_ma.value"
};

struct NodeDef {
    std::string id;
    std::string op_type;      // "SMA", "EMA", "CROSS_ABOVE", "THRESHOLD", etc.
    std::unordered_map<std::string, PortDef> inputs;
    std::unordered_map<std::string, PortDef> outputs;
    std::unordered_map<std::string, double>  params;
};

struct EdgeDef {
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
};

struct DataBinding {
    std::string data_source;  // "kline.close", "kline.open", etc.
    std::string to_node;
    std::string to_port;
};

struct SignalHandler {
    std::string signal_node;
    std::string handler_type; // "order", "alert"
    std::unordered_map<std::string, double> params;
};

struct StrategyGraph {
    std::string strategy_name;
    uint32_t    version = 1;
    std::vector<NodeDef>         nodes;
    std::vector<EdgeDef>         edges;
    std::vector<DataBinding>     data_bindings;
    std::vector<SignalHandler>   signal_handlers;

    // Load from JSON file
    static StrategyGraph load_from_file(const std::string& path);

    // Load from JSON string
    static StrategyGraph load_from_json(const std::string& json_str);

    // Serialize to JSON string
    std::string to_json() const;

    // Write to JSON file
    void write_to_file(const std::string& path) const;

    // Validate: check type compatibility, no cycles, all ports connected
    bool validate(std::string& error_msg) const;

    // Lookup helpers
    const NodeDef* find_node(const std::string& id) const;
    std::vector<const NodeDef*> nodes_by_op(const std::string& op_type) const;
};

}  // namespace quant::ir
