// strategy_api.h — Strategy submission/registration API
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cpp/quant/backtest/backtest_runner.h"
#include "cpp/quant/strategy/strategy_registry.h"

namespace quant::storage { class StorageEngine; }
namespace quant::strategy { class StrategyEngine; }

namespace quant::api {

struct ApiResponse {
    int status_code = 200;
    std::string body;
};

struct BacktestHistoryEntry {
    int64_t timestamp;
    std::string result_json;
};

class StrategyApi {
public:
    StrategyApi(strategy::StrategyEngine& engine,
                backtest::BacktestRunner& runner,
                storage::StorageEngine& storage);

    ApiResponse handle_request(const std::string& method,
                                const std::string& path,
                                const std::string& body = "",
                                const std::string& query = "");

private:
    ApiResponse list_strategies();
    ApiResponse get_strategy(uint64_t id);
    ApiResponse register_strategy(const std::string& body);
    ApiResponse update_strategy(uint64_t id, const std::string& body);
    ApiResponse delete_strategy(uint64_t id);
    ApiResponse activate_strategy(uint64_t id);
    ApiResponse pause_strategy(uint64_t id);
    ApiResponse trigger_backtest(uint64_t id, const std::string& body);
    ApiResponse batch_backtest(const std::string& body);
    ApiResponse backtest_history(uint64_t id);
    ApiResponse clone_strategy(uint64_t id);

    ApiResponse start_live_strategy(uint64_t id);
    ApiResponse stop_live_strategy(uint64_t id);
    ApiResponse live_status(uint64_t id);
    ApiResponse live_list();

    ApiResponse handle_data(const std::string& method,
                             const std::vector<std::string>& segments,
                             const std::string& body,
                             const std::string& query);
    ApiResponse handle_symbols();

    std::string entry_to_json(const strategy::StrategyEntry& entry);
    std::string result_to_json(const backtest::BacktestResult& result);

    strategy::StrategyEngine& engine_;
    backtest::BacktestRunner& runner_;
    storage::StorageEngine& storage_;

    // key: strategy_id, value: list of history entries (newest first)
    std::unordered_map<uint64_t, std::vector<BacktestHistoryEntry>> history_;
};

}  // namespace quant::api
