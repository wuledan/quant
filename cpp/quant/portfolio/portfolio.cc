// portfolio.cc — Portfolio implementation
#include "cpp/quant/portfolio/portfolio.h"

namespace quant::portfolio {

Portfolio::Portfolio(double initial_cash)
    : initial_cash_(initial_cash), cash_(initial_cash) {}

void Portfolio::on_fill(const std::string& symbol, execution::OrderSide side,
                        double quantity, double price, double commission) {
    auto& pos = positions_[symbol];
    pos.symbol = symbol;

    if (side == execution::OrderSide::kBuy) {
        double total_cost = pos.avg_cost * pos.quantity + price * quantity;
        pos.quantity += quantity;
        pos.avg_cost = (pos.quantity > 0) ? total_cost / pos.quantity : 0.0;
        cash_ -= price * quantity + commission;
    } else {
        pos.quantity -= quantity;
        if (pos.quantity <= 1e-9) {
            pos.quantity = 0.0;
            pos.avg_cost = 0.0;
        }
        cash_ += price * quantity - commission;
    }
    pos.market_value = pos.quantity * price;
}

void Portfolio::update_market_value(const std::string& symbol, double current_price) {
    auto it = positions_.find(symbol);
    if (it != positions_.end()) {
        it->second.market_value = it->second.quantity * current_price;
    }
}

double Portfolio::cash() const noexcept { return cash_; }

double Portfolio::position_value() const noexcept {
    double total = 0.0;
    for (const auto& [_, pos] : positions_) {
        total += pos.market_value;
    }
    return total;
}

double Portfolio::total_value() const noexcept {
    return cash_ + position_value();
}

double Portfolio::unrealized_pnl() const noexcept {
    double pnl = 0.0;
    for (const auto& [_, pos] : positions_) {
        pnl += (pos.market_value - pos.avg_cost * pos.quantity);
    }
    return pnl;
}

const Position* Portfolio::get_position(const std::string& symbol) const {
    auto it = positions_.find(symbol);
    return (it != positions_.end()) ? &it->second : nullptr;
}

const std::unordered_map<std::string, Position>& Portfolio::positions() const noexcept {
    return positions_;
}

void Portfolio::record_nav(int64_t timestamp) {
    nav_history_.emplace_back(timestamp, total_value());
}

const std::vector<std::pair<int64_t, double>>& Portfolio::nav_history() const noexcept {
    return nav_history_;
}

}  // namespace quant::portfolio
