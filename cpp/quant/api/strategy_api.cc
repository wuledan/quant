// strategy_api.cc — Strategy submission/registration API implementation
#include "cpp/quant/api/strategy_api.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "cpp/quant/backtest/backtest_runner.h"
#include "cpp/quant/infra/logging/logger.h"
#include "cpp/quant/ir/ir_graph.h"
#include "cpp/quant/storage/column_block.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/strategy/strategy_engine.h"
#include "cpp/quant/strategy/strategy_registry.h"

namespace quant::api {
namespace {

using infra::default_logger;

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
                                          const std::string& body,
                                          const std::string& query) {
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

    // Expect: ["api", ...]
    if (segments.size() < 2 || segments[0] != "api") {
        return error_response(404, "Not found");
    }

    // ── Market data routes: /api/data/* ──
    // /api/data/scheduler_status (before general data handler)
    if (segments[1] == "data" && segments.size() >= 3 && segments[2] == "scheduler_status") {
        JsonWriter w;
        w.begin_obj();
        w.key("status"); w.str_val("running"); w.comma();
        w.key("active_tasks"); w.int_val(0); w.comma();
        w.key("queued_tasks"); w.int_val(0);
        w.end_obj();
        return success_response(w.os.str());
    }
    // /api/data/daily/:symbol (before general data handler)
    if (segments[1] == "data" && segments.size() >= 4 && segments[2] == "daily") {
        // Parse date range from query: start_date=2026-05-01&end_date=2026-05-26
        int64_t range_start = 0, range_end = INT64_MAX;
        auto parse_date = [](const std::string& s) -> int64_t {
            if (s.size() < 10) return 0;
            struct tm tm = {};
            tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
            tm.tm_mon = std::stoi(s.substr(5, 2)) - 1;
            tm.tm_mday = std::stoi(s.substr(8, 2));
            return static_cast<int64_t>(timegm(&tm)) * 1'000'000;
        };
        for (size_t i = 0; i + 6 < query.size(); ) {
            size_t eq = query.find('=', i), amp = query.find('&', eq);
            if (eq == std::string::npos) break;
            std::string key = query.substr(i, eq - i);
            std::string val = query.substr(eq + 1, amp == std::string::npos ? query.size() - eq - 1 : amp - eq - 1);
            if (key == "start_date") range_start = parse_date(val);
            if (key == "end_date") range_end = parse_date(val) + 86400LL * 1'000'000; // end of day
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
        JsonWriter w;
        w.begin_arr();
        auto rows = storage_.query_kline(segments[3], 7, range_start, range_end);
        for (size_t i = 0; i < rows.size(); ++i) {
            w.begin_obj();
            w.key("timestamp"); w.int_val(rows[i].timestamp / 1000); w.comma();
            w.key("open"); w.num_val(static_cast<double>(rows[i].open_price) / 10000.0); w.comma();
            w.key("high"); w.num_val(static_cast<double>(rows[i].high_price) / 10000.0); w.comma();
            w.key("low"); w.num_val(static_cast<double>(rows[i].low_price) / 10000.0); w.comma();
            w.key("close"); w.num_val(static_cast<double>(rows[i].close_price) / 10000.0); w.comma();
            w.key("volume"); w.num_val(static_cast<double>(rows[i].volume));
            w.end_obj();
            if (i + 1 < rows.size()) w.comma();
        }
        w.end_arr();
        return success_response(w.os.str());
    }
    if (segments[1] == "data") {
        return handle_data(method, segments, body, query);
    }

    // ── Symbols: /api/symbols ──
    if (segments[1] == "symbols") {
        return handle_symbols();
    }

    // ── Legacy routes + factor compute ──
    // POST /api/factors/compute?symbol=600519.SH
    if (segments[1] == "factors" && segments.size() >= 3 && segments[2] == "compute" && method == "POST") {
        std::string sym;
        for (size_t i = 0; i + 6 < query.size(); ) {
            size_t eq = query.find('=', i), amp = query.find('&', eq);
            if (eq == std::string::npos) break;
            std::string key = query.substr(i, eq - i);
            std::string val = query.substr(eq + 1, amp == std::string::npos ? query.size() - eq - 1 : amp - eq - 1);
            if (key == "symbol") sym = val;
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
        if (sym.empty()) return error_response(400, "Missing symbol");
        auto rows = storage_.query_kline(sym, 7, 0, INT64_MAX);
        if (rows.empty()) return error_response(404, "No data for symbol");

        // Build input data vector
        std::vector<double> close_prices, volumes;
        for (auto& r : rows) {
            close_prices.push_back(static_cast<double>(r.close_price) / 10000.0);
            volumes.push_back(static_cast<double>(r.volume));
        }

        // Compute basic technical indicators
        JsonWriter w;
        w.begin_obj();
        w.key("symbol"); w.str_val(sym); w.comma();
        w.key("bars"); w.int_val(static_cast<int64_t>(rows.size())); w.comma();
        w.key("factors"); w.begin_obj();

        auto compute_ma = [&](int period) {
            w.key("SMA_" + std::to_string(period)); w.begin_arr();
            double sum = 0;
            for (size_t i = 0; i < close_prices.size(); ++i) {
                sum += close_prices[i];
                if (i >= static_cast<size_t>(period)) sum -= close_prices[i - period];
                w.num_val(i >= static_cast<size_t>(period - 1) ? sum / period : 0);
                if (i + 1 < close_prices.size()) w.comma();
            }
            w.end_arr(); w.comma();
        };

        compute_ma(5);
        compute_ma(10);
        compute_ma(20);
        compute_ma(60);

        w.key("bars"); w.int_val(static_cast<int64_t>(rows.size()));
        w.end_obj(); w.comma();
        w.key("count"); w.int_val(static_cast<int64_t>(rows.size()));
        w.end_obj();
        return success_response(w.os.str());
    }
    // GET /api/factors/list
    if (segments[1] == "factors" && segments.size() >= 3 && segments[2] == "list" && method == "GET") {
        JsonWriter w;
        w.begin_obj();
        w.key("factors"); w.begin_arr();
        const char* fnames[] = {"SMA_5","SMA_10","SMA_20","SMA_60","EMA_10","EMA_30","RSI_14"};
        for (size_t i = 0; i < 7; ++i) {
            w.str_val(fnames[i]);
            if (i < 6) w.comma();
        }
        w.end_arr();
        w.end_obj();
        return success_response(w.os.str());
    }

    // GET /api/strategy/list
    if (segments[1] == "strategy" && segments.size() >= 3 && segments[2] == "list") {
        return list_strategies();
    }
    // GET /api/backtest/list, /api/backtest/:id/status
    if (segments[1] == "backtest") {
        JsonWriter w;
        if (segments.size() >= 3 && segments[2] == "list") {
            w.begin_arr(); w.end_arr();
            return success_response(w.os.str());
        }
        if (segments.size() >= 4 && segments[3] == "status") {
            w.begin_obj();
            w.key("status"); w.str_val("completed"); w.comma();
            w.key("progress"); w.num_val(100);
            w.end_obj();
            return success_response(w.os.str());
        }
        // POST /api/backtest/run
        if (segments.size() >= 3 && segments[2] == "run" && method == "POST") {
            w.begin_obj();
            w.key("ok"); w.os << "true"; w.comma();
            w.key("status"); w.str_val("submitted");
            w.end_obj();
            return success_response(w.os.str());
        }
        return error_response(404, "Not found");
    }
    // ── Strategy routes: /api/strategies/* ──
    if (segments[1] != "strategies") {
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

    // POST /api/strategies/batch-backtest — batch backtest (intercept before ID parsing)
    if (segments.size() == 3 && segments[2] == "batch-backtest" && method == "POST") {
        return batch_backtest(body);
    }

    // GET /api/strategies/live — list all live strategies
    if (segments.size() == 3 && segments[2] == "live" && method == "GET") {
        return live_list();
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
        if (action == "activate" && method == "POST")         return activate_strategy(id);
        if (action == "pause" && method == "POST")            return pause_strategy(id);
        if (action == "backtest" && method == "POST")         return trigger_backtest(id, body);
        if (action == "backtest-history" && method == "GET")  return backtest_history(id);
        if (action == "clone" && method == "POST")            return clone_strategy(id);
        if (action == "start_live" && method == "POST")       return start_live_strategy(id);
        if (action == "stop_live" && method == "POST")        return stop_live_strategy(id);
        if (action == "live_status" && method == "GET")       return live_status(id);
        return error_response(404, "Not found");
    }

    return error_response(404, "Not found");
}

// ── Individual handlers ──

ApiResponse StrategyApi::list_strategies() {
    auto entries = engine_.registry().list_strategies();

    JsonWriter w;
    w.begin_arr();
    for (size_t i = 0; i < entries.size(); ++i) {
        w.os << entry_to_json(entries[i]);
        if (i + 1 < entries.size()) w.comma();
    }
    w.end_arr();

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
    std::string graph_content;  // JSON string of the .graph IR
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
            } else if (k == "graph_content") {
                graph_content = p.parse_string();
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

    // If graph_content is provided, write it to ./data/graphs/{name}.graph
    // and use that path for registration. This allows Python to upload the
    // compiled IR JSON directly instead of requiring a pre-existing file.
    if (!graph_content.empty()) {
        std::string graphs_dir = "./data/graphs";
        // Ensure directory exists
        std::string mkdir_cmd = "mkdir -p " + graphs_dir;
        (void)std::system(mkdir_cmd.c_str());

        graph_path = graphs_dir + "/" + name + ".graph";
        std::ofstream ofs(graph_path);
        if (!ofs.is_open()) {
            return error_response(500, "Failed to write graph file: " + graph_path);
        }
        ofs << graph_content;
        ofs.close();
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
                    params.kline_type = static_cast<uint8_t>(p.parse_number());
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
    std::string result_json = result_to_json(result);

    // Record history entry
    BacktestHistoryEntry hist_entry;
    hist_entry.timestamp = std::time(nullptr);
    hist_entry.result_json = result_json;
    history_[id].insert(history_[id].begin(), std::move(hist_entry));

    JsonWriter w;
    w.begin_obj();
    w.key("strategy_id"); w.int_val(static_cast<int64_t>(id)); w.comma();
    w.key("result");
    w.os << result_json;
    w.end_obj();

    return success_response(w.os.str());
}

// ── Batch backtest ──

ApiResponse StrategyApi::batch_backtest(const std::string& body) {
    JsonParser p{body.data(), body.size(), 0};

    std::vector<uint64_t> strategy_ids;
    backtest::BacktestParams params;

    try {
        p.expect('{');
        while (true) {
            auto k = p.next_key();
            if (k.empty()) break;
            p.expect(':');
            if (k == "strategy_ids") {
                p.expect('[');
                p.skip_ws();
                while (p.peek() != ']') {
                    strategy_ids.push_back(
                        static_cast<uint64_t>(p.parse_number()));
                    if (p.peek() == ',') p.next();
                }
                p.expect(']');
            } else if (k == "params") {
                // Parse backtest params object
                p.expect('{');
                while (true) {
                    auto pk = p.next_key();
                    if (pk.empty()) break;
                    p.expect(':');
                    if (pk == "initial_cash") {
                        params.initial_cash = p.parse_number();
                    } else if (pk == "start_time") {
                        params.start_time = static_cast<int64_t>(p.parse_number());
                    } else if (pk == "end_time") {
                        params.end_time = static_cast<int64_t>(p.parse_number());
                    } else if (pk == "symbol") {
                        params.symbol = p.parse_string();
                    } else {
                        p.skip_value();
                    }
                }
                p.expect('}');
            } else {
                p.skip_value();
            }
        }
        p.expect('}');
    } catch (const std::exception& e) {
        return error_response(400, std::string("Invalid JSON: ") + e.what());
    }

    if (strategy_ids.empty()) {
        return error_response(400, "Empty strategy_ids");
    }

    JsonWriter w;
    w.begin_obj();
    w.key("results");
    w.begin_arr();
    for (size_t i = 0; i < strategy_ids.size(); ++i) {
        auto sid = strategy_ids[i];
        auto* entry = engine_.registry().find(sid);

        w.begin_obj();
        w.key("strategy_id");
        w.int_val(static_cast<int64_t>(sid));
        w.comma();

        if (!entry) {
            w.key("error");
            w.str_val("Strategy not found");
        } else if (entry->graph_path.empty()) {
            w.key("error");
            w.str_val("No graph_path configured");
        } else {
            try {
                auto result = runner_.run(entry->graph_path, params);
                std::string result_json = result_to_json(result);

                // Record history for this strategy
                BacktestHistoryEntry hist_entry;
                hist_entry.timestamp = std::time(nullptr);
                hist_entry.result_json = result_json;
                history_[sid].insert(history_[sid].begin(), std::move(hist_entry));

                w.key("result");
                w.os << result_json;
            } catch (const std::exception& e) {
                w.key("error");
                w.str_val(std::string("Backtest failed: ") + e.what());
            }
        }

        w.end_obj();
        if (i + 1 < strategy_ids.size()) w.comma();
    }
    w.end_arr();
    w.end_obj();

    return success_response(w.os.str());
}

// ── Backtest history ──

ApiResponse StrategyApi::backtest_history(uint64_t id) {
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(404, "Strategy not found");
    }

    JsonWriter w;
    w.begin_obj();
    w.key("history");
    w.begin_arr();

    auto it = history_.find(id);
    if (it != history_.end()) {
        const auto& entries = it->second;
        for (size_t i = 0; i < entries.size(); ++i) {
            w.begin_obj();
            w.key("timestamp");
            w.int_val(entries[i].timestamp);
            w.comma();
            w.key("result");
            w.os << entries[i].result_json;
            w.end_obj();
            if (i + 1 < entries.size()) w.comma();
        }
    }

    w.end_arr();
    w.end_obj();

    return success_response(w.os.str());
}

// ── Clone strategy ──

ApiResponse StrategyApi::clone_strategy(uint64_t id) {
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(404, "Strategy not found");
    }

    std::string clone_name = entry->name + "-copy";
    auto new_id = engine_.registry().register_strategy(
        clone_name, entry->graph_path, entry->params);

    auto* new_entry = engine_.registry().find(new_id);
    if (!new_entry) {
        return error_response(500, "Failed to clone strategy");
    }

    return {201, entry_to_json(*new_entry)};
}

// ── Live strategy management ──

ApiResponse StrategyApi::start_live_strategy(uint64_t id) {
    auto* entry = engine_.registry().find(id);
    if (!entry) {
        return error_response(404, "Strategy not found");
    }
    if (entry->graph_path.empty()) {
        return error_response(400, "No graph_path configured");
    }

    // Load IR graph
    ir::StrategyGraph graph;
    try {
        default_logger().info("Loading graph from: " + entry->graph_path);
        graph = ir::StrategyGraph::load_from_file(entry->graph_path);
        default_logger().info("Graph loaded: strategy_name=" + graph.strategy_name +
                              ", nodes=" + std::to_string(graph.nodes.size()) +
                              ", edges=" + std::to_string(graph.edges.size()));
    } catch (const std::exception& e) {
        default_logger().error("Failed to load graph from '" + entry->graph_path +
                               "': " + e.what());
        return error_response(500, std::string("Failed to load graph: ") + e.what());
    }

    auto result = engine_.start_live(id, graph);
    if (!result.has_value()) {
        default_logger().error("Cannot start live strategy id=" + std::to_string(id) +
                               ": start_live returned nullopt");
        return error_response(409, "Cannot start live strategy");
    }

    return live_status(id);
}

ApiResponse StrategyApi::stop_live_strategy(uint64_t id) {
    engine_.stop_live(id);

    JsonWriter w;
    w.begin_obj();
    w.key("stopped"); w.os << "true"; w.comma();
    w.key("strategy_id"); w.int_val(static_cast<int64_t>(id));
    w.end_obj();
    return success_response(w.os.str());
}

ApiResponse StrategyApi::live_status(uint64_t id) {
    auto status = engine_.live_status(id);

    JsonWriter w;
    w.begin_obj();
    w.key("is_running"); w.os << (status.is_running ? "true" : "false"); w.comma();

    if (status.latest_signal.has_value()) {
        w.key("latest_signal"); w.begin_obj();
        w.key("timestamp"); w.int_val(status.latest_signal->timestamp); w.comma();
        w.key("symbol"); w.str_val(status.latest_signal->symbol); w.comma();
        w.key("price"); w.num_val(status.latest_signal->price); w.comma();
        w.key("side"); w.int_val(status.latest_signal->side); w.comma();
        w.key("quantity"); w.int_val(status.latest_signal->quantity); w.comma();
        w.key("confidence"); w.num_val(status.latest_signal->confidence);
        w.end_obj(); w.comma();
    }

    w.key("factors"); w.begin_obj();
    size_t fi = 0;
    for (const auto& [k, v] : status.factors) {
        w.key(k); w.num_val(v);
        if (++fi < status.factors.size()) w.comma();
    }
    w.end_obj();

    w.end_obj();
    return success_response(w.os.str());
}

ApiResponse StrategyApi::live_list() {
    auto ids = engine_.live_strategies();

    JsonWriter w;
    w.begin_arr();
    for (size_t i = 0; i < ids.size(); ++i) {
        auto status = engine_.live_status(ids[i]);
        w.begin_obj();
        w.key("strategy_id"); w.int_val(static_cast<int64_t>(ids[i])); w.comma();
        w.key("is_running"); w.os << (status.is_running ? "true" : "false");
        w.end_obj();
        if (i + 1 < ids.size()) w.comma();
    }
    w.end_arr();
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
    w.end_arr(); w.comma();

    w.key("trades"); w.begin_arr();
    for (size_t i = 0; i < result.trades.size(); ++i) {
        const auto& t = result.trades[i];
        w.begin_obj();
        w.key("timestamp"); w.int_val(t.timestamp); w.comma();
        w.key("price"); w.num_val(t.price); w.comma();
        w.key("side"); w.str_val(t.side); w.comma();
        w.key("quantity"); w.int_val(t.quantity);
        w.end_obj();
        if (i + 1 < result.trades.size()) w.comma();
    }
    w.end_arr();

    w.end_obj();
    return w.os.str();
}

// ── Market data handlers ──

ApiResponse StrategyApi::handle_data(const std::string& method,
                                      const std::vector<std::string>& segments,
                                      const std::string& body,
                                      const std::string& query) {
    // GET /api/data/kline?symbol=&interval=&start=&end=
    if (segments.size() >= 3 && segments[2] == "kline" && method == "GET") {
        // Parse query string: symbol=600519.SH&interval=1d
        std::string symbol;
        for (size_t i = 0; i + 6 < query.size(); ) {
            size_t eq = query.find('=', i);
            size_t amp = query.find('&', eq);
            if (eq == std::string::npos) break;
            std::string key = query.substr(i, eq - i);
            std::string val = query.substr(eq + 1, (amp == std::string::npos ? query.size() : amp) - eq - 1);
            if (key == "symbol") symbol = val;
            if (amp == std::string::npos) break;
            i = amp + 1;
        }

        // Parse date range from query
        int64_t range_start = 0, range_end = INT64_MAX;
        auto parse_date = [](const std::string& s) -> int64_t {
            if (s.size() < 10) return 0;
            struct tm tm = {};
            tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
            tm.tm_mon = std::stoi(s.substr(5, 2)) - 1;
            tm.tm_mday = std::stoi(s.substr(8, 2));
            return static_cast<int64_t>(timegm(&tm)) * 1'000'000;
        };
        for (size_t j = 0; j + 6 < query.size(); ) {
            size_t eq = query.find('=', j), amp = query.find('&', eq);
            if (eq == std::string::npos) break;
            std::string key = query.substr(j, eq - j);
            std::string val = query.substr(eq + 1, amp == std::string::npos ? query.size() - eq - 1 : amp - eq - 1);
            if (key == "start_date") range_start = parse_date(val);
            if (key == "end_date") range_end = parse_date(val) + 86400LL * 1'000'000;
            if (amp == std::string::npos) break;
            j = amp + 1;
        }

        JsonWriter w;
        w.begin_arr();
        if (!symbol.empty()) {
            auto rows = storage_.query_kline(symbol, static_cast<uint8_t>(storage::KlineFreq::kDay), range_start, range_end);
            for (size_t i = 0; i < rows.size(); ++i) {
                w.begin_obj();
                w.key("timestamp"); w.int_val(rows[i].timestamp / 1000); w.comma();
                w.key("open"); w.num_val(static_cast<double>(rows[i].open_price) / 10000.0); w.comma();
                w.key("high"); w.num_val(static_cast<double>(rows[i].high_price) / 10000.0); w.comma();
                w.key("low"); w.num_val(static_cast<double>(rows[i].low_price) / 10000.0); w.comma();
                w.key("close"); w.num_val(static_cast<double>(rows[i].close_price) / 10000.0); w.comma();
                w.key("volume"); w.num_val(static_cast<double>(rows[i].volume));
                w.end_obj();
                if (i + 1 < rows.size()) w.comma();
            }
        }
        w.end_arr();
        return success_response(w.os.str());
    }
    // GET /api/data/ticker?symbol=
    if (segments.size() >= 3 && segments[2] == "ticker" && method == "GET") {
        JsonWriter w;
        w.begin_obj();
        w.key("symbol"); w.str_val(""); w.comma();
        w.key("price"); w.num_val(0); w.comma();
        w.key("change"); w.num_val(0); w.comma();
        w.key("volume"); w.int_val(0); w.comma();
        w.key("timestamp"); w.int_val(0);
        w.end_obj();
        return success_response(w.os.str());
    }
    return error_response(404, "Not found");
}

ApiResponse StrategyApi::handle_symbols() {
    JsonWriter w;
    w.begin_arr();

    const char* syms[] = {"000001.SZ", "000002.SZ", "300750.SZ",
                          "600519.SH", "000001.SH", "000300.SH",
                          "399001.SZ", "399006.SZ"};
    static const size_t n = sizeof(syms) / sizeof(syms[0]);
    for (size_t i = 0; i < n; ++i) {
        w.begin_obj();
        w.key("symbol"); w.str_val(syms[i]); w.comma();
        w.key("name"); w.str_val(syms[i]); w.comma();
        w.key("exchange"); w.str_val(
            std::string(syms[i]).find(".SH") != std::string::npos ? "SH" : "SZ");
        w.end_obj();
        if (i + 1 < n) w.comma();
    }

    w.end_arr();
    return success_response(w.os.str());
}

}  // namespace quant::api
