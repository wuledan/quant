// signal_handler.h — Signal handler interface and implementations
//
// When a signal node outputs a non-zero value, a SignalHandler is invoked
// to perform the appropriate action (place order, send alert, etc.).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace quant::execution { class OrderManager; }
namespace quant::event { class EventBus; }

namespace quant::strategy {

// ── Signal context: provides market data and portfolio state ──

struct SignalContext {
    std::string symbol;
    double price = 0.0;        // current price (float, in yuan)
    double position = 0.0;     // current position (shares)
    double cash = 0.0;         // available cash
    int64_t timestamp = 0;     // bar timestamp
    double signal_value = 0.0; // the signal that triggered this handler
};

// ── Signal handler interface ──

class ISignalHandler {
public:
    virtual ~ISignalHandler() = default;
    virtual void handle(double signal_value, const SignalContext& ctx) = 0;
    virtual std::string handler_type() const = 0;
};

// ── Order signal handler: generates buy/sell orders based on signal ──

class OrderSignalHandler : public ISignalHandler {
public:
    struct Params {
        double buy_weight = 0.95;    // fraction of cash to use for buying
        double sell_weight = 1.0;    // fraction of position to sell
        double min_signal = 0.0;     // minimum absolute signal to act on
    };

    OrderSignalHandler(Params params,
                       execution::OrderManager& order_mgr,
                       event::EventBus& bus);

    void handle(double signal_value, const SignalContext& ctx) override;
    std::string handler_type() const override { return "order"; }

    // Statistics
    int64_t orders_generated() const { return orders_generated_; }

private:
    Params params_;
    execution::OrderManager& order_mgr_;
    event::EventBus& bus_;
    int64_t orders_generated_ = 0;
};

// ── Alert signal handler: publishes risk alert events ──

class AlertSignalHandler : public ISignalHandler {
public:
    explicit AlertSignalHandler(event::EventBus& bus);

    void handle(double signal_value, const SignalContext& ctx) override;
    std::string handler_type() const override { return "alert"; }

    int64_t alerts_sent() const { return alerts_sent_; }

private:
    event::EventBus& bus_;
    int64_t alerts_sent_ = 0;
};

// ── Signal handler factory: creates handlers from IR SignalHandler defs ──

class SignalHandlerFactory {
public:
    using HandlerCreator = std::function<std::unique_ptr<ISignalHandler>(
        const std::unordered_map<std::string, double>& params,
        execution::OrderManager& order_mgr,
        event::EventBus& bus)>;

    static void register_handler_type(const std::string& type, HandlerCreator creator);
    static std::unique_ptr<ISignalHandler> create(
        const std::string& type,
        const std::unordered_map<std::string, double>& params,
        execution::OrderManager& order_mgr,
        event::EventBus& bus);

private:
    static std::unordered_map<std::string, HandlerCreator>& registry();
};

}  // namespace quant::strategy