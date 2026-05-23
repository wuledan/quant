// postgres_store.cc — PostgreSQL metadata and backtest results store
//
// Uses psql subprocess for SQL execution. Follows the etcd_client
// subprocess pattern — no libpq dependency required.
#include "cpp/quant/storage/postgres_store.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace quant::storage {

// ── Constructor / Destructor ──

PostgresStore::PostgresStore(Options opts)
    : opts_(std::move(opts)) {}

PostgresStore::~PostgresStore() = default;

// ── Connection string ──

std::string PostgresStore::conn_string() const {
    std::string s;
    s += "postgresql://";
    s += opts_.user;
    if (!opts_.password.empty()) {
        s += ':';
        // Percent-encode colons in password
        for (char c : opts_.password) {
            if (c == ':') s += "%3A";
            else if (c == '@') s += "%40";
            else if (c == '/') s += "%2F";
            else if (c == '%') s += "%25";
            else s += c;
        }
    }
    s += '@';
    s += opts_.host;
    s += ':';
    s += std::to_string(opts_.port);
    s += '/';
    s += opts_.dbname;
    return s;
}

// ── Subprocess helper ──

int PostgresStore::run_psql(const std::string& sql, std::string* output) {
    std::string cmd = "psql '";
    // Escape single quotes in connection string for shell
    std::string conn = conn_string();
    for (size_t i = 0; i < conn.size(); ++i) {
        if (conn[i] == '\'') cmd += "'\\''";
        else cmd += conn[i];
    }
    cmd += "' -c '";
    // Escape single quotes in SQL
    for (size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '\'') cmd += "'\\''";
        else cmd += sql[i];
    }
    cmd += "' -q 2>&1";

    std::string captured;
    std::array<char, 4096> buffer;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -1;

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        captured += buffer.data();
    }

    int exit_code = pclose(pipe);
    int status = WEXITSTATUS(exit_code);

    if (output) *output = captured;
    return status;
}

// ── Connection ──

bool PostgresStore::connect() {
    std::string output;
    int rc = run_psql("SELECT 1", &output);
    connected_ = (rc == 0);
    return connected_;
}

// ── Schema ──

bool PostgresStore::init_schema() {
    if (schema_inited_) return true;
    if (!connected_) return false;

    std::string sql = R"SQL(
        CREATE TABLE IF NOT EXISTS backtest_results (
            id BIGSERIAL PRIMARY KEY,
            strategy_id BIGINT,
            symbol VARCHAR(16),
            start_ts BIGINT,
            end_ts BIGINT,
            total_return DOUBLE PRECISION,
            annual_return DOUBLE PRECISION,
            max_drawdown DOUBLE PRECISION,
            sharpe_ratio DOUBLE PRECISION,
            total_trades BIGINT,
            nav_json TEXT,
            created_at TIMESTAMP DEFAULT NOW()
        );
        CREATE TABLE IF NOT EXISTS symbols (
            symbol VARCHAR(16) PRIMARY KEY,
            name VARCHAR(64),
            market VARCHAR(8),
            status VARCHAR(8) DEFAULT 'ACTIVE'
        );
    )SQL";

    int rc = run_psql(sql);
    schema_inited_ = (rc == 0);
    return schema_inited_;
}

// ── Backtest results ──

bool PostgresStore::insert_backtest_result(uint64_t strategy_id,
                                            const std::string& symbol,
                                            int64_t start_ts, int64_t end_ts,
                                            double total_return,
                                            double annual_return,
                                            double max_drawdown,
                                            double sharpe_ratio,
                                            int64_t total_trades,
                                            const std::string& nav_json) {
    if (!connected_) return false;

    std::string sql = "INSERT INTO backtest_results "
        "(strategy_id, symbol, start_ts, end_ts, total_return, "
        "annual_return, max_drawdown, sharpe_ratio, total_trades, nav_json) "
        "VALUES (" +
        std::to_string(strategy_id) + ", '" +
        symbol + "', " +
        std::to_string(start_ts) + ", " +
        std::to_string(end_ts) + ", " +
        std::to_string(total_return) + ", " +
        std::to_string(annual_return) + ", " +
        std::to_string(max_drawdown) + ", " +
        std::to_string(sharpe_ratio) + ", " +
        std::to_string(total_trades) + ", '" +
        nav_json + "')";

    return run_psql(sql) == 0;
}

std::vector<BacktestRow> PostgresStore::query_backtest_results(
    uint64_t strategy_id, int limit) {
    if (!connected_) return {};

    std::string sql = "SELECT id, strategy_id, symbol, start_ts, end_ts, "
        "total_return, annual_return, max_drawdown, sharpe_ratio, "
        "total_trades, COALESCE(nav_json, ''), "
        "COALESCE(created_at::text, '') "
        "FROM backtest_results WHERE strategy_id = " +
        std::to_string(strategy_id) +
        " ORDER BY id DESC LIMIT " + std::to_string(limit);

    // Use --csv for machine-parseable output
    std::string cmd = "psql '";
    std::string conn = conn_string();
    for (size_t i = 0; i < conn.size(); ++i) {
        if (conn[i] == '\'') cmd += "'\\''";
        else cmd += conn[i];
    }
    cmd += "' --csv -c '";
    for (size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '\'') cmd += "'\\''";
        else cmd += sql[i];
    }
    cmd += "' 2>/dev/null";

    std::string output;
    std::array<char, 4096> buffer;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);

    if (output.empty()) return {};

    // Parse CSV: first line is header (skip), subsequent lines are data
    // Format: id,strategy_id,symbol,start_ts,end_ts,total_return,...
    std::vector<BacktestRow> results;
    std::istringstream stream(output);
    std::string line;

    // Skip header
    std::getline(stream, line);

    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        BacktestRow row{};

        // Simple CSV parsing (no embedded commas in our fields except nav_json)
        std::vector<std::string> fields;
        std::string field;
        bool in_quotes = false;
        for (char c : line) {
            if (c == '"') {
                in_quotes = !in_quotes;
            } else if (c == ',' && !in_quotes) {
                fields.push_back(std::move(field));
                field.clear();
            } else {
                field += c;
            }
        }
        fields.push_back(std::move(field));

        // Expect 12 fields
        if (fields.size() < 12) continue;

        auto safe_stoll = [](const std::string& s) -> int64_t {
            if (s.empty()) return 0;
            try { return std::stoll(s); } catch (...) { return 0; }
        };
        auto safe_stod = [](const std::string& s) -> double {
            if (s.empty()) return 0.0;
            try { return std::stod(s); } catch (...) { return 0.0; }
        };

        row.id = safe_stoll(fields[0]);
        row.strategy_id = static_cast<uint64_t>(safe_stoll(fields[1]));
        row.symbol = fields[2];
        row.start_ts = safe_stoll(fields[3]);
        row.end_ts = safe_stoll(fields[4]);
        row.total_return = safe_stod(fields[5]);
        row.annual_return = safe_stod(fields[6]);
        row.max_drawdown = safe_stod(fields[7]);
        row.sharpe_ratio = safe_stod(fields[8]);
        row.total_trades = safe_stoll(fields[9]);
        row.nav_json = fields[10];
        row.created_at = fields[11];

        results.push_back(std::move(row));
    }

    return results;
}

// ── Symbol metadata ──

bool PostgresStore::insert_symbol(const std::string& symbol,
                                   const std::string& name,
                                   const std::string& market,
                                   const std::string& status) {
    if (!connected_) return false;

    std::string sql = "INSERT INTO symbols (symbol, name, market, status) "
        "VALUES ('" + symbol + "', '" + name + "', '" +
        market + "', '" + status + "') "
        "ON CONFLICT (symbol) DO UPDATE SET "
        "name = EXCLUDED.name, market = EXCLUDED.market, "
        "status = EXCLUDED.status";

    return run_psql(sql) == 0;
}

std::vector<std::string> PostgresStore::list_symbols(const std::string& status) {
    if (!connected_) return {};

    std::string sql = "SELECT symbol FROM symbols";
    if (!status.empty()) {
        sql += " WHERE status = '" + status + "'";
    }
    sql += " ORDER BY symbol";

    std::string output;
    int rc = run_psql(sql, &output);
    if (rc != 0 || output.empty()) return {};

    // psql -c output includes:
    //   symbol\n-------\n000001\n000002\n(2 rows)\n
    // We need to extract just the symbol lines.
    std::vector<std::string> symbols;
    std::istringstream stream(output);
    std::string line;

    // Skip header line and separator line
    bool header_passed = false;
    bool separator_passed = false;

    while (std::getline(stream, line)) {
        // Trim trailing whitespace
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        if (!header_passed) {
            header_passed = true;
            continue;
        }
        if (!separator_passed) {
            // Separator line is all dashes
            if (!line.empty() && line[0] == '-') {
                separator_passed = true;
                continue;
            }
            // No separator means single column with no header decoration
            separator_passed = true;
        }

        // Skip summary line like "(N rows)"
        if (line.front() == '(' && line.back() == ')') continue;

        symbols.push_back(line);
    }

    return symbols;
}

}  // namespace quant::storage
