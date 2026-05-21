// portfolio.h — Portfolio tracking: positions, cash, NAV
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/execution/order.h"

namespace quant::portfolio {

struct Position {
    std::string symbol;
    double quantity = 0.0;
    double avg_cost = 0.0;
    double market_value = 0.0;
};

class Portfolio {
public:
    explicit Portfolio(double initial_cash);

    void on_fill(const std::string& symbol, execution::OrderSide side,
                 double quantity, double price, double commission);

    void update_market_value(const std::string& symbol, double current_price);

    double cash() const noexcept;
    double total_value() const noexcept;
    double position_value() const noexcept;
    double unrealized_pnl() const noexcept;
    const Position* get_position(const std::string& symbol) const;
    const std::unordered_map<std::string, Position>& positions() const noexcept;

    void record_nav(int64_t timestamp);
    const std::vector<std::pair<int64_t, double>>& nav_history() const noexcept;

private:
    double initial_cash_;
    double cash_;
    std::unordered_map<std::string, Position> positions_;
    std::vector<std::pair<int64_t, double>> nav_history_;
};

}  // namespace quant::portfolio
