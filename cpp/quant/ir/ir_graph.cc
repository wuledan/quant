// ir_graph.cc — StrategyGraph JSON serialization and validation
#include "cpp/quant/ir/ir_graph.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace quant::ir {
namespace {

// ── Minimal JSON serializer ──

struct JsonWriter {
    std::ostringstream os;
    int indent = 0;

    void key(const std::string& k) {
        os << std::string(indent * 2, ' ') << '"' << k << "\": ";
    }

    void begin_obj() { os << "{\n"; ++indent; }
    void end_obj()   { --indent; os << "\n" << std::string(indent * 2, ' ') << "}"; }
    void begin_arr() { os << "[\n"; ++indent; }
    void end_arr()   { --indent; os << "\n" << std::string(indent * 2, ' ') << "]"; }
    void comma()     { os << ","; }
    void nl()        { os << "\n"; }

    void str_val(const std::string& v) {
        os << '"';
        for (char c : v) {
            if (c == '"') os << "\\\"";
            else if (c == '\\') os << "\\\\";
            else os << c;
        }
        os << '"';
    }

    void num_val(double v) {
        if (std::isnan(v) || std::isinf(v)) {
            os << "null";
        } else if (v == static_cast<int64_t>(v) && std::abs(v) < 1e15) {
            os << static_cast<int64_t>(v);
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.15g", v);
            os << buf;
        }
    }

    void int_val(int64_t v) { os << v; }
};

void write_type_spec(JsonWriter& w, const TypeSpec& ts) {
    w.begin_obj();
    w.key("base_type"); w.str_val(ts.base_type); w.comma(); w.nl();
    w.key("inner_type"); w.str_val(ts.inner_type); w.nl();
    w.end_obj();
}

void write_port_def(JsonWriter& w, const PortDef& p) {
    w.begin_obj();
    w.key("name"); w.str_val(p.name); w.comma(); w.nl();
    w.key("type"); write_type_spec(w, p.type); w.comma(); w.nl();
    w.key("source"); w.str_val(p.source); w.nl();
    w.end_obj();
}

void write_node_def(JsonWriter& w, const NodeDef& n, bool is_last) {
    w.begin_obj();
    w.key("id"); w.str_val(n.id); w.comma(); w.nl();
    w.key("op_type"); w.str_val(n.op_type); w.comma(); w.nl();

    w.key("inputs"); w.begin_obj();
    size_t i = 0;
    for (const auto& [k, v] : n.inputs) {
        w.key(k); write_port_def(w, v);
        if (++i < n.inputs.size()) w.comma();
        w.nl();
    }
    w.end_obj(); w.comma(); w.nl();

    w.key("outputs"); w.begin_obj();
    i = 0;
    for (const auto& [k, v] : n.outputs) {
        w.key(k); write_port_def(w, v);
        if (++i < n.outputs.size()) w.comma();
        w.nl();
    }
    w.end_obj(); w.comma(); w.nl();

    w.key("params"); w.begin_obj();
    i = 0;
    for (const auto& [k, v] : n.params) {
        w.key(k); w.num_val(v);
        if (++i < n.params.size()) w.comma();
        w.nl();
    }
    w.end_obj(); w.nl();

    w.end_obj();
    if (!is_last) w.comma();
    w.nl();
}

void write_edge_def(JsonWriter& w, const EdgeDef& e, bool is_last) {
    w.begin_obj();
    w.key("from_node"); w.str_val(e.from_node); w.comma(); w.nl();
    w.key("from_port"); w.str_val(e.from_port); w.comma(); w.nl();
    w.key("to_node"); w.str_val(e.to_node); w.comma(); w.nl();
    w.key("to_port"); w.str_val(e.to_port); w.nl();
    w.end_obj();
    if (!is_last) w.comma();
    w.nl();
}

void write_data_binding(JsonWriter& w, const DataBinding& b, bool is_last) {
    w.begin_obj();
    w.key("data_source"); w.str_val(b.data_source); w.comma(); w.nl();
    w.key("to_node"); w.str_val(b.to_node); w.comma(); w.nl();
    w.key("to_port"); w.str_val(b.to_port); w.nl();
    w.end_obj();
    if (!is_last) w.comma();
    w.nl();
}

void write_signal_handler(JsonWriter& w, const SignalHandler& h, bool is_last) {
    w.begin_obj();
    w.key("signal_node"); w.str_val(h.signal_node); w.comma(); w.nl();
    w.key("handler_type"); w.str_val(h.handler_type); w.comma(); w.nl();

    w.key("params"); w.begin_obj();
    size_t i = 0;
    for (const auto& [k, v] : h.params) {
        w.key(k); w.num_val(v);
        if (++i < h.params.size()) w.comma();
        w.nl();
    }
    w.end_obj(); w.nl();

    w.end_obj();
    if (!is_last) w.comma();
    w.nl();
}

// ── Minimal JSON parser ──

struct JsonParser {
    const char* data;
    size_t len;
    size_t pos = 0;

    void skip_ws() {
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t' ||
                             data[pos] == '\n' || data[pos] == '\r'))
            ++pos;
    }

    char peek() {
        skip_ws();
        return pos < len ? data[pos] : '\0';
    }

    char next() {
        skip_ws();
        return pos < len ? data[pos++] : '\0';
    }

    void expect(char c) {
        if (next() != c)
            throw std::runtime_error(
                std::string("Expected '") + c + "' at pos " + std::to_string(pos));
    }

    std::string parse_string() {
        expect('"');
        std::string result;
        while (pos < len && data[pos] != '"') {
            if (data[pos] == '\\') {
                ++pos;
                if (pos < len) {
                    if (data[pos] == 'n') result += '\n';
                    else if (data[pos] == 't') result += '\t';
                    else result += data[pos];
                }
            } else {
                result += data[pos];
            }
            ++pos;
        }
        expect('"');
        return result;
    }

    double parse_number() {
        skip_ws();
        size_t start = pos;
        if (pos < len && (data[pos] == '-' || data[pos] == '+')) ++pos;
        while (pos < len && (std::isdigit(data[pos]) || data[pos] == '.' ||
                             data[pos] == 'e' || data[pos] == 'E' ||
                             data[pos] == '-' || data[pos] == '+'))
            ++pos;
        return std::stod(std::string(data + start, pos - start));
    }

    bool parse_bool() {
        if (pos + 4 <= len && std::strncmp(data + pos, "true", 4) == 0) {
            pos += 4; return true;
        }
        if (pos + 5 <= len && std::strncmp(data + pos, "false", 5) == 0) {
            pos += 5; return false;
        }
        throw std::runtime_error("Expected bool at pos " + std::to_string(pos));
    }

    void parse_null() {
        if (pos + 4 <= len && std::strncmp(data + pos, "null", 4) == 0) {
            pos += 4; return;
        }
        throw std::runtime_error("Expected null at pos " + std::to_string(pos));
    }

    // Returns the next key in an object, or empty string if at end
    std::string next_key() {
        skip_ws();
        if (peek() == '}') return "";
        if (peek() == ',') { next(); skip_ws(); }
        return parse_string();
    }

    TypeSpec parse_type_spec() {
        TypeSpec ts;
        expect('{');
        while (true) {
            auto k = next_key();
            if (k.empty()) break;
            expect(':');
            if (k == "base_type") ts.base_type = parse_string();
            else if (k == "inner_type") ts.inner_type = parse_string();
            else skip_value();
        }
        expect('}');
        return ts;
    }

    PortDef parse_port_def() {
        PortDef p;
        expect('{');
        while (true) {
            auto k = next_key();
            if (k.empty()) break;
            expect(':');
            if (k == "name") p.name = parse_string();
            else if (k == "type") p.type = parse_type_spec();
            else if (k == "source") p.source = parse_string();
            else skip_value();
        }
        expect('}');
        return p;
    }

    NodeDef parse_node_def() {
        NodeDef n;
        expect('{');
        while (true) {
            auto k = next_key();
            if (k.empty()) break;
            expect(':');
            if (k == "id") n.id = parse_string();
            else if (k == "op_type") n.op_type = parse_string();
            else if (k == "inputs") {
                expect('{');
                while (true) {
                    auto ik = next_key();
                    if (ik.empty()) break;
                    expect(':');
                    n.inputs[ik] = parse_port_def();
                }
                expect('}');
            } else if (k == "outputs") {
                expect('{');
                while (true) {
                    auto ok = next_key();
                    if (ok.empty()) break;
                    expect(':');
                    n.outputs[ok] = parse_port_def();
                }
                expect('}');
            } else if (k == "params") {
                expect('{');
                while (true) {
                    auto pk = next_key();
                    if (pk.empty()) break;
                    expect(':');
                    n.params[pk] = parse_number();
                }
                expect('}');
            } else {
                skip_value();
            }
        }
        expect('}');
        return n;
    }

    EdgeDef parse_edge_def() {
        EdgeDef e;
        expect('{');
        while (true) {
            auto k = next_key();
            if (k.empty()) break;
            expect(':');
            if (k == "from_node") e.from_node = parse_string();
            else if (k == "from_port") e.from_port = parse_string();
            else if (k == "to_node") e.to_node = parse_string();
            else if (k == "to_port") e.to_port = parse_string();
            else skip_value();
        }
        expect('}');
        return e;
    }

    DataBinding parse_data_binding() {
        DataBinding b;
        expect('{');
        while (true) {
            auto k = next_key();
            if (k.empty()) break;
            expect(':');
            if (k == "data_source") b.data_source = parse_string();
            else if (k == "to_node") b.to_node = parse_string();
            else if (k == "to_port") b.to_port = parse_string();
            else skip_value();
        }
        expect('}');
        return b;
    }

    SignalHandler parse_signal_handler() {
        SignalHandler h;
        expect('{');
        while (true) {
            auto k = next_key();
            if (k.empty()) break;
            expect(':');
            if (k == "signal_node") h.signal_node = parse_string();
            else if (k == "handler_type") h.handler_type = parse_string();
            else if (k == "params") {
                expect('{');
                while (true) {
                    auto pk = next_key();
                    if (pk.empty()) break;
                    expect(':');
                    h.params[pk] = parse_number();
                }
                expect('}');
            } else skip_value();
        }
        expect('}');
        return h;
    }

    StrategyGraph parse_graph() {
        StrategyGraph g;
        expect('{');
        while (true) {
            auto k = next_key();
            if (k.empty()) break;
            expect(':');
            if (k == "strategy_name") g.strategy_name = parse_string();
            else if (k == "version") g.version = static_cast<uint32_t>(parse_number());
            else if (k == "nodes") {
                expect('[');
                while (peek() != ']') {
                    g.nodes.push_back(parse_node_def());
                    if (peek() == ',') next();
                }
                expect(']');
            } else if (k == "edges") {
                expect('[');
                while (peek() != ']') {
                    g.edges.push_back(parse_edge_def());
                    if (peek() == ',') next();
                }
                expect(']');
            } else if (k == "data_bindings") {
                expect('[');
                while (peek() != ']') {
                    g.data_bindings.push_back(parse_data_binding());
                    if (peek() == ',') next();
                }
                expect(']');
            } else if (k == "signal_handlers") {
                expect('[');
                while (peek() != ']') {
                    g.signal_handlers.push_back(parse_signal_handler());
                    if (peek() == ',') next();
                }
                expect(']');
            } else skip_value();
        }
        expect('}');
        return g;
    }

    void skip_value() {
        skip_ws();
        char c = peek();
        if (c == '"') parse_string();
        else if (c == '{') { next(); while (next_key() != "") skip_value(), expect(':'), skip_value(); expect('}'); }
        else if (c == '[') { next(); skip_ws(); while (peek() != ']') { skip_value(); if (peek() == ',') next(); } expect(']'); }
        else if (c == 't' || c == 'f') parse_bool();
        else if (c == 'n') parse_null();
        else parse_number();
    }
};

}  // anonymous namespace

// ── Serialization ──

std::string StrategyGraph::to_json() const {
    JsonWriter w;
    w.begin_obj();

    w.key("strategy_name"); w.str_val(strategy_name); w.comma(); w.nl();
    w.key("version"); w.int_val(version); w.comma(); w.nl();

    w.key("nodes"); w.begin_arr();
    for (size_t i = 0; i < nodes.size(); ++i)
        write_node_def(w, nodes[i], i == nodes.size() - 1);
    w.end_arr(); w.comma(); w.nl();

    w.key("edges"); w.begin_arr();
    for (size_t i = 0; i < edges.size(); ++i)
        write_edge_def(w, edges[i], i == edges.size() - 1);
    w.end_arr(); w.comma(); w.nl();

    w.key("data_bindings"); w.begin_arr();
    for (size_t i = 0; i < data_bindings.size(); ++i)
        write_data_binding(w, data_bindings[i], i == data_bindings.size() - 1);
    w.end_arr(); w.comma(); w.nl();

    w.key("signal_handlers"); w.begin_arr();
    for (size_t i = 0; i < signal_handlers.size(); ++i)
        write_signal_handler(w, signal_handlers[i], i == signal_handlers.size() - 1);
    w.end_arr(); w.nl();

    w.end_obj();
    return w.os.str();
}

void StrategyGraph::write_to_file(const std::string& path) const {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write IR to: " + path);
    f << to_json();
}

// ── Deserialization ──

StrategyGraph StrategyGraph::load_from_json(const std::string& json_str) {
    JsonParser p{json_str.data(), json_str.size(), 0};
    return p.parse_graph();
}

StrategyGraph StrategyGraph::load_from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot read IR from: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return load_from_json(ss.str());
}

// ── Validation ──

bool StrategyGraph::validate(std::string& error_msg) const {
    // Check all node IDs are unique
    std::unordered_map<std::string, size_t> id_count;
    for (const auto& n : nodes) id_count[n.id]++;
    for (const auto& [id, cnt] : id_count) {
        if (cnt > 1) {
            error_msg = "Duplicate node id: " + id;
            return false;
        }
    }

    // Check edges reference existing nodes
    for (const auto& e : edges) {
        if (!find_node(e.from_node)) {
            error_msg = "Edge references non-existent from_node: " + e.from_node;
            return false;
        }
        if (!find_node(e.to_node)) {
            error_msg = "Edge references non-existent to_node: " + e.to_node;
            return false;
        }
    }

    // Check data_bindings reference existing nodes
    for (const auto& b : data_bindings) {
        if (!find_node(b.to_node)) {
            error_msg = "DataBinding references non-existent to_node: " + b.to_node;
            return false;
        }
    }

    // Check signal_handlers reference existing nodes
    for (const auto& h : signal_handlers) {
        if (!find_node(h.signal_node)) {
            error_msg = "SignalHandler references non-existent signal_node: " + h.signal_node;
            return false;
        }
    }

    // Cycle detection via DFS
    std::unordered_map<std::string, std::vector<std::string>> adj;
    for (const auto& e : edges) adj[e.from_node].push_back(e.to_node);

    enum State { kUnvisited, kVisiting, kVisited };
    std::unordered_map<std::string, State> state;
    for (const auto& n : nodes) state[n.id] = kUnvisited;

    std::function<bool(const std::string&)> has_cycle = [&](const std::string& id) -> bool {
        state[id] = kVisiting;
        for (const auto& dep : adj[id]) {
            if (state[dep] == kVisiting) return true;
            if (state[dep] == kUnvisited && has_cycle(dep)) return true;
        }
        state[id] = kVisited;
        return false;
    };

    for (const auto& n : nodes) {
        if (state[n.id] == kUnvisited && has_cycle(n.id)) {
            error_msg = "Cycle detected in graph involving node: " + n.id;
            return false;
        }
    }

    return true;
}

// ── Lookup helpers ──

const NodeDef* StrategyGraph::find_node(const std::string& id) const {
    for (const auto& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

std::vector<const NodeDef*> StrategyGraph::nodes_by_op(const std::string& op_type) const {
    std::vector<const NodeDef*> result;
    for (const auto& n : nodes) {
        if (n.op_type == op_type) result.push_back(&n);
    }
    return result;
}

}  // namespace quant::ir