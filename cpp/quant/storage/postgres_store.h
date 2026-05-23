// postgres_store.h — PostgreSQL metadata and backtest results store
//
// Uses psql subprocess for reliability (libpq headers not available in
// build environment). Follows the same pattern as etcd_client (etcdctl
// subprocess). All methods are synchronous (blocking) — call from a
// background thread when used in coroutine context.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace quant::storage {

// ── Backtest result row ──
struct BacktestRow {
    int64_t id;
    uint64_t strategy_id;
    std::string symbol;
    int64_t start_ts;
    int64_t end_ts;
    double total_return;
    double annual_return;
    double max_drawdown;
    double sharpe_ratio;
    int64_t total_trades;
    std::string nav_json;
    std::string created_at;
};

// ── PostgresStore ──
class PostgresStore {
public:
    struct Options {
        std::string host = "127.0.0.1";
        int port = 5432;
        std::string dbname = "quant_invest";
        std::string user = "quant";
        std::string password;
    };

    explicit PostgresStore(Options opts);
    ~PostgresStore();

    PostgresStore(const PostgresStore&) = delete;
    PostgresStore& operator=(const PostgresStore&) = delete;

    // ── Connection ──
    bool connect();
    bool is_connected() const noexcept { return connected_; }

    // ── Schema ──
    bool init_schema();

    // ── Backtest results ──
    bool insert_backtest_result(uint64_t strategy_id,
                                 const std::string& symbol,
                                 int64_t start_ts, int64_t end_ts,
                                 double total_return, double annual_return,
                                 double max_drawdown, double sharpe_ratio,
                                 int64_t total_trades,
                                 const std::string& nav_json);

    std::vector<BacktestRow> query_backtest_results(uint64_t strategy_id,
                                                     int limit = 50);

    // ── Symbol metadata ──
    bool insert_symbol(const std::string& symbol, const std::string& name,
                       const std::string& market, const std::string& status);

    std::vector<std::string> list_symbols(const std::string& status = "ACTIVE");

private:
    int run_psql(const std::string& sql, std::string* output = nullptr);
    std::string conn_string() const;

    Options opts_;
    bool connected_{false};
    bool schema_inited_{false};
};

}  // namespace quant::storage
