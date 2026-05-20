// signal_handler.cc — Signal handler implementations
#include "cpp/quant/strategy/signal_handler.h"

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/risk_alert_event.h"
#include "cpp/quant/execution/order_manager.h"
#include "cpp/quant/execution/order.h"

namespace quant::strategy {

// ── OrderSignalHandler ──

OrderSignalHandler::OrderSignalHandler(
    Params params,
    execution::OrderManager& order_mgr,
    event::EventBus& bus)
    : params_(params), order_mgr_(order_mgr), bus_(bus) {}

void OrderSignalHandler::handle(double signal_value, const SignalContext& ctx) {
    if (std::abs(signal_value) < params_.min_signal) return;

    execution::OrderRequest req;
    req.symbol = ctx.symbol;

    if (signal_value > 0) {
        // Buy signal: use buy_weight fraction of cash
        double qty = ctx.cash * params_.buy_weight / ctx.price;
        if (qty > 0) {
            req.side = execution::OrderSide::kBuy;
            req.price = static_cast<int64_t>(ctx.price * 10000);
            req.quantity = static_cast<int64_t>(qty);
            order_mgr_.create_order(req);
            ++orders_generated_;
        }
    } else if (signal_value < 0) {
        // Sell signal: sell sell_weight fraction of position
        double qty = ctx.position * params_.sell_weight;
        if (qty > 0) {
            req.side = execution::OrderSide::kSell;
            req.price = static_cast<int64_t>(ctx.price * 10000);
            req.quantity = static_cast<int64_t>(qty);
            order_mgr_.create_order(req);
            ++orders_generated_;
        }
    }
}

// ── AlertSignalHandler ──

AlertSignalHandler::AlertSignalHandler(event::EventBus& bus) : bus_(bus) {}

void AlertSignalHandler::handle(double signal_value, const SignalContext& ctx) {
    auto alert = std::make_unique<event::RiskAlertEvent>();
    alert->message = "Signal alert: " + ctx.symbol +
                     " signal=" + std::to_string(signal_value) +
                     " price=" + std::to_string(ctx.price);
    alert->severity = event::RuleSeverity::kInfo;
    alert->risk_level = event::RiskLevel::kYellow;
    alert->rule_id = 0;
    bus_.publish(std::move(alert));
    ++alerts_sent_;
}

// ── SignalHandlerFactory ──

std::unordered_map<std::string, SignalHandlerFactory::HandlerCreator>&
SignalHandlerFactory::registry() {
    static std::unordered_map<std::string, SignalHandlerFactory::HandlerCreator> r;
    return r;
}

void SignalHandlerFactory::register_handler_type(
    const std::string& type, HandlerCreator creator) {
    registry()[type] = std::move(creator);
}

std::unique_ptr<ISignalHandler> SignalHandlerFactory::create(
    const std::string& type,
    const std::unordered_map<std::string, double>& params,
    execution::OrderManager& order_mgr,
    event::EventBus& bus) {
    // Register built-in types on first use
    static bool initialized = false;
    if (!initialized) {
        register_handler_type("order",
            [](const auto& p, auto& om, auto& eb) {
                OrderSignalHandler::Params op;
                op.buy_weight = p.count("buy_weight") ? p.at("buy_weight") : 0.95;
                op.sell_weight = p.count("sell_weight") ? p.at("sell_weight") : 1.0;
                op.min_signal = p.count("min_signal") ? p.at("min_signal") : 0.0;
                return std::make_unique<OrderSignalHandler>(op, om, eb);
            });
        register_handler_type("alert",
            [](const auto&, auto&, auto& eb) {
                return std::make_unique<AlertSignalHandler>(eb);
            });
        initialized = true;
    }

    auto it = registry().find(type);
    if (it == registry().end()) return nullptr;
    return it->second(params, order_mgr, bus);
}

}  // namespace quant::strategy