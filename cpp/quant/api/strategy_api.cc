// strategy_api.cc — Strategy submission/registration API implementation
#include "cpp/quant/api/strategy_api.h"

#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "cpp/quant/backtest/backtest_runner.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/strategy/strategy_engine.h"
#include "cpp/quant/strategy/strategy_registry.h"

namespace quant::api {
namespace {

// ── Minimal JSON serializer (same pattern as ir_graph.cc) ──

struct JsonWriter {
    std::ostringstream os;

    void key(const std::string& k) {
        os << '"' << k << "\":";
    }

    void begin_obj() { os << "{"; }
    void end_obj()   { os << "}"; }
    void begin_arr() { os << "["; }
    void end_arr()   { os << "]"; }
    void comma()     { os << ","; }

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

// ── Minimal JSON parser (same pattern as ir_graph.cc) ──

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

    void skip_value() {
        skip_ws();
        char c = peek();
        if (c == '"') { parse_string(); }
        else if (c == '{') {
            next();
            while (next_key() != "") { expect(':'); skip_value(); }
            expect('}');
        } else if (c == '[') {
            next(); skip_ws();
            while (peek() != ']') { skip_value(); if (peek() == ',') next(); }
            expect(']');
        } else if (c == 't' || c == 'f') {
            if (pos + 4 <= len && std::strncmp(data + pos, "true", 4) == 0) pos += 4;
            else if (pos + 5 <= len && std::strncmp(data + pos, "false", 5) == 0) pos += 5;
        } else if (c == 'n') {
            pos += 4;  // null
        } else {
            parse_number();
        }
    }

    std::string next_key() {
        skip_ws();
        if (peek() == '}') return "";
        if (peek() == ',') { next(); skip_ws(); }
        return parse_string();
    }
};

// ── Helper functions ──

ApiResponse error_response(int status, const std::string& msg) {
    JsonWriter w;
    w.begin_obj();
    w.key("error"); w.str_val(msg);
    w.end_obj();
    return {status, w.os.str()};
}

ApiResponse success_response(const std::string& json_body) {
    return {200, json_body};
}

std::unordered_map<std::string, double> parse_params(JsonParser& p) {
    std::unordered_map<std::string, double> params;
    p.expect('{');
    while (true) {
        auto k = p.next_key();
        if (k.empty()) break;
        p.expect(':');
        params[k] = p.parse_number();
    }
    p.expect('}');
    return params;
}

void write_params(JsonWriter& w, const std::unordered_map<std::string, double>& params) {
    w.begin_obj();
    size_t i = 0;
    for (const auto& [k, v] : params) {
        w.key(k); w.num_val(v);
        if (++i < params.size()) w.comma();
    }
    w.end_obj();
}

const char* status_to_string(strategy::StrategyStatus s) {
    switch (s) {
        case strategy::StrategyStatus::kDraft:   return "draft";
        case strategy::StrategyStatus::kActive:  return "active";
        case strategy::StrategyStatus::kPaused:  return "paused";
        case strategy::StrategyStatus::kDeleted: return "deleted";
    }
    return "unknown";
}

uint64_t parse_id(const std::string& s) {
    try {
        return std::stoull(s);
    } catch (...) {
        return 0;
    }
}

}  // anonymous namespace

// ── Constructor ──

StrategyApi::StrategyApi(strategy::StrategyEngine& engine,
                          backtest::BacktestRunner& runner,
                          storage::StorageEngine& storage)
    : engine_(engine), runner_(runner), storage_(storage) {}

// ── Request dispatcher ──

ApiResponse StrategyApi::handle_request(const std::string& method,
                                          const std::string& path,
                                          const std::string& body) {
    // Parse path segments
    std::vector<std::string> segments;
    std::string seg;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/') {
            if (!seg.empty()) {
                segments.push_back(seg);
                seg.clear();
            }
        } else {
            seg += path[i];
        }
    }
    if (!seg.empty()) segments.push_back(seg);

    // Expect: ["api", "strategies", ...]
    if (segments.size() < 2 || segments[0] != "api" || segments[1] != "strategies") {
        return error_response(404, "Not found");
    }

    // GET /api/strategies
    if (segments.size() == 2 && method == "GET") {
        return list_strategies();
    }

    // POST /api/strategies
    if (segments.size() == 2 && method == "POST") {
        return register_strategy(body);
    }

    // Need an ID at position 2
    if (segments.size() < 3) {
        return error_response(400, "Missing strategy ID");
    }

    uint64_t id = parse_id(segments[2]);
    if (id == 0) {
        return error_response(400, "Invalid strategy ID");
    }

    // /api/strategies/:id
    if (segments.size() == 3) {
        if (method == "GET")    return get_strategy(id);
        if (method == "PUT")    return update_strategy(id, body);
        if (method == "DELETE") return delete_strategy(id);
        return error_response(405, "Method not allowed");
    }

    // /api/strategies/:id/<action>
    if (segments.size() == 4) {
        const auto& action = segments[3];
        if (action == "activate" && method == "POST") return activate_strategy(id);
        if (action == "pause" && method == "POST")    return pause_strategy(id);
        if (action == "backtest" && method == "POST") return trigger_backtest(id, body);
        return error_response(404, "Not found");
    }

    return error_response(404, "Not found");
}

// ── Individual handlers ──

ApiResponse StrategyApi::list_strategies() {
    auto entries = engine_.registry().list_strategies();

    JsonWriter w;
    w.begin_obj();
    w.key("strategies"); w.begin_arr();
    for (size_t i = 0; i < entries.size(); ++i) {
        w.os << entry_to_json(entries[i]);
        if (i + 1 < entries.size()) w.comma();
    }
    w.end_arr();
    w.end_obj();

    return success_response(w.os.str());
}

ApiResponse StrategyApi::get_strategy(uint64_t id) {
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(404, "Strategy not found");
    }
    return success_response(entry_to_json(*entry));
}

ApiResponse StrategyApi::register_strategy(const std::string& body) {
    JsonParser p{body.data(), body.size(), 0};

    std::string name;
    std::string graph_path;
    std::unordered_map<std::string, double> params;

    try {
        p.expect('{');
        while (true) {
            auto k = p.next_key();
            if (k.empty()) break;
            p.expect(':');
            if (k == "name") {
                name = p.parse_string();
            } else if (k == "graph_path") {
                graph_path = p.parse_string();
            } else if (k == "params") {
                params = parse_params(p);
            } else {
                p.skip_value();
            }
        }
        p.expect('}');
    } catch (const std::exception& e) {
        return error_response(400, std::string("Invalid JSON: ") + e.what());
    }

    if (name.empty()) {
        return error_response(400, "Missing required field: name");
    }

    auto id = engine_.registry().register_strategy(name, graph_path, params);
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(500, "Failed to register strategy");
    }

    return {201, entry_to_json(*entry)};
}

ApiResponse StrategyApi::update_strategy(uint64_t id, const std::string& body) {
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(404, "Strategy not found");
    }

    JsonParser p{body.data(), body.size(), 0};

    std::string graph_path;
    std::unordered_map<std::string, double> params;
    bool has_graph_path = false;
    bool has_params = false;

    try {
        p.expect('{');
        while (true) {
            auto k = p.next_key();
            if (k.empty()) break;
            p.expect(':');
            if (k == "graph_path") {
                graph_path = p.parse_string();
                has_graph_path = true;
            } else if (k == "params") {
                params = parse_params(p);
                has_params = true;
            } else {
                p.skip_value();
            }
        }
        p.expect('}');
    } catch (const std::exception& e) {
        return error_response(400, std::string("Invalid JSON: ") + e.what());
    }

    if (has_graph_path) {
        engine_.registry().update_graph_path(id, graph_path);
    }
    if (has_params) {
        engine_.registry().update_params(id, params);
    }

    auto* updated = engine_.registry().find(id);
    return success_response(entry_to_json(*updated));
}

ApiResponse StrategyApi::delete_strategy(uint64_t id) {
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(404, "Strategy not found");
    }

    bool ok = engine_.registry().remove_strategy(id);
    if (!ok) {
        return error_response(500, "Failed to delete strategy");
    }

    JsonWriter w;
    w.begin_obj();
    w.key("deleted"); w.os << "true";
    w.end_obj();
    return success_response(w.os.str());
}

ApiResponse StrategyApi::activate_strategy(uint64_t id) {
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(404, "Strategy not found");
    }

    bool ok = engine_.activate(id);
    if (!ok) {
        return error_response(409, "Cannot activate strategy (already active or invalid graph)");
    }

    auto* updated = engine_.registry().find(id);
    return success_response(entry_to_json(*updated));
}

ApiResponse StrategyApi::pause_strategy(uint64_t id) {
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(404, "Strategy not found");
    }

    bool ok = engine_.pause(id);
    if (!ok) {
        return error_response(409, "Cannot pause strategy (not active)");
    }

    auto* updated = engine_.registry().find(id);
    return success_response(entry_to_json(*updated));
}

ApiResponse StrategyApi::trigger_backtest(uint64_t id, const std::string& body) {
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(404, "Strategy not found");
    }

    if (entry->graph_path.empty()) {
        return error_response(400, "No graph_path configured");
    }

    backtest::BacktestParams params;

    if (!body.empty()) {
        JsonParser p{body.data(), body.size(), 0};
        try {
            p.expect('{');
            while (true) {
                auto k = p.next_key();
                if (k.empty()) break;
                p.expect(':');
                if (k == "initial_cash") {
                    params.initial_cash = p.parse_number();
                } else if (k == "start_time") {
                    params.start_time = static_cast<int64_t>(p.parse_number());
                } else if (k == "end_time") {
                    params.end_time = static_cast<int64_t>(p.parse_number());
                } else if (k == "symbol") {
                    params.symbol = p.parse_string();
                } else if (k == "kline_type") {
                    params.kline_type = static_cast<event::DataType>(
                        static_cast<int>(p.parse_number()));
                } else {
                    p.skip_value();
                }
            }
            p.expect('}');
        } catch (const std::exception& e) {
            return error_response(400, std::string("Invalid JSON: ") + e.what());
        }
    }

    // Run backtest
    backtest::BacktestResult result;
    try {
        result = runner_.run(entry->graph_path, params);
    } catch (const std::exception& e) {
        return error_response(500, std::string("Backtest failed: ") + e.what());
    }

    // Build response with strategy_id and result
    JsonWriter w;
    w.begin_obj();
    w.key("strategy_id"); w.int_val(static_cast<int64_t>(id)); w.comma();
    w.key("result");
    w.os << result_to_json(result);
    w.end_obj();

    return success_response(w.os.str());
}

// ── JSON serialization ──

std::string StrategyApi::entry_to_json(const strategy::StrategyEntry& entry) {
    JsonWriter w;
    w.begin_obj();
    w.key("id"); w.int_val(static_cast<int64_t>(entry.id)); w.comma();
    w.key("name"); w.str_val(entry.name); w.comma();
    w.key("graph_path"); w.str_val(entry.graph_path); w.comma();
    w.key("status"); w.str_val(status_to_string(entry.status)); w.comma();
    w.key("params"); write_params(w, entry.params); w.comma();
    w.key("created_at"); w.int_val(entry.created_at); w.comma();
    w.key("updated_at"); w.int_val(entry.updated_at);
    w.end_obj();
    return w.os.str();
}

std::string StrategyApi::result_to_json(const backtest::BacktestResult& result) {
    JsonWriter w;
    w.begin_obj();
    w.key("total_return"); w.num_val(result.total_return); w.comma();
    w.key("annual_return"); w.num_val(result.annual_return); w.comma();
    w.key("max_drawdown"); w.num_val(result.max_drawdown); w.comma();
    w.key("sharpe_ratio"); w.num_val(result.sharpe_ratio); w.comma();
    w.key("total_trades"); w.int_val(result.total_trades); w.comma();

    w.key("nav_curve"); w.begin_arr();
    for (size_t i = 0; i < result.nav_curve.size(); ++i) {
        const auto& [ts, nav] = result.nav_curve[i];
        w.begin_obj();
        w.key("timestamp"); w.int_val(ts); w.comma();
        w.key("nav"); w.num_val(nav);
        w.end_obj();
        if (i + 1 < result.nav_curve.size()) w.comma();
    }
    w.end_arr();

    w.end_obj();
    return w.os.str();
}

}  // namespace quant::api
