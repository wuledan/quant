// simulated_broker.h — Simulated order execution for backtesting
#pragma once

#include <cstdint>
#include <string>

#include "cpp/quant/execution/order.h"

namespace quant::backtest {

struct SimulatedFill {
    std::string symbol;
    execution::OrderSide side;
    double quantity = 0.0;
    double price = 0.0;
    double commission = 0.0;
    int64_t timestamp = 0;
};

struct SimulatedBrokerConfig {
    double commission_rate = 0.0003;
    double min_commission = 5.0;
    double slippage_bps = 1.0;
    bool enable_slippage = true;
};

class SimulatedBroker {
public:
    using Config = SimulatedBrokerConfig;

    explicit SimulatedBroker(Config config = {});

    SimulatedFill execute(const std::string& symbol,
                          execution::OrderSide side,
                          double quantity,
                          double market_price,
                          int64_t timestamp);

    const Config& config() const noexcept;

private:
    Config config_;
};

}  // namespace backtest
