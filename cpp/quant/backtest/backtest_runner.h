// backtest_runner.h — Backtest orchestrator: loads IR, runs strategy, produces results
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/storage/storage_engine.h"

namespace quant::ir { struct StrategyGraph; }

namespace quant::backtest {

struct BacktestParams {
    double initial_cash = 1000000.0;
    int64_t start_time = 0;
    int64_t end_time = 0;
    std::string symbol;
    event::DataType kline_type = event::DataType::kKlineDay;
};

struct BacktestResult {
    double total_return = 0.0;
    double annual_return = 0.0;
    double max_drawdown = 0.0;
    double sharpe_ratio = 0.0;
    int64_t total_trades = 0;
    std::vector<std::pair<int64_t, double>> nav_curve;
};

class BacktestRunner {
public:
    BacktestRunner(storage::StorageEngine& storage, event::EventBus& bus);

    BacktestResult run(const std::string& graph_path, const BacktestParams& params);
    BacktestResult run(const ir::StrategyGraph& graph, const BacktestParams& params);

private:
    storage::StorageEngine& storage_;
    event::EventBus& bus_;
};

}  // namespace backtest
