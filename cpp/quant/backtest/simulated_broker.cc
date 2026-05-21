// simulated_broker.cc — Simulated broker implementation
#include "cpp/quant/backtest/simulated_broker.h"

namespace quant::backtest {

SimulatedBroker::SimulatedBroker(Config config) : config_(config) {}

SimulatedFill SimulatedBroker::execute(const std::string& symbol,
                                        execution::OrderSide side,
                                        double quantity,
                                        double market_price,
                                        int64_t timestamp) {
    double fill_price = market_price;

    if (config_.enable_slippage) {
        double slip = market_price * config_.slippage_bps / 10000.0;
        if (side == execution::OrderSide::kBuy) {
            fill_price += slip;
        } else {
            fill_price -= slip;
        }
    }

    double commission = fill_price * quantity * config_.commission_rate;
    if (commission < config_.min_commission) {
        commission = config_.min_commission;
    }

    return SimulatedFill{symbol, side, quantity, fill_price, commission, timestamp};
}

const SimulatedBroker::Config& SimulatedBroker::config() const noexcept {
    return config_;
}

}  // namespace backtest
