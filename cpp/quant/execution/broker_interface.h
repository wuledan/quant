// broker_interface.h — Broker abstraction interface for order routing
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cpp/quant/execution/order.h"

namespace quant::execution {

// ── Connection status ──
enum class ConnectionStatus : uint8_t {
    kDisconnected = 0,
    kConnecting    = 1,
    kConnected     = 2,
    kAuthenticating = 3,
    kAuthenticated  = 4,
    kError         = 5,
};

// ── Broker configuration ──
struct BrokerConfig {
    std::string broker_id;
    std::string endpoint;
    std::string api_key;
    std::string api_secret;
    int32_t     timeout_ms = 5000;
    bool        auto_reconnect = true;
    int32_t     reconnect_interval_ms = 3000;
};

// ── Broker callback interface ──
class IBrokerCallback {
public:
    virtual ~IBrokerCallback() = default;

    virtual void on_connected() = 0;
    virtual void on_disconnected(const std::string& reason) = 0;
    virtual void on_order_accepted(OrderId order_id, const std::string& broker_order_id) = 0;
    virtual void on_order_rejected(OrderId order_id, const std::string& reason) = 0;
    virtual void on_order_filled(OrderId order_id, int64_t fill_qty, int64_t fill_price) = 0;
    virtual void on_order_cancelled(OrderId order_id) = 0;
    virtual void on_order_expired(OrderId order_id) = 0;
};

// ── Broker abstract interface ──
class IBroker {
public:
    virtual ~IBroker() = default;

    // ── Connection lifecycle ──
    virtual ConnectionStatus connect() = 0;
    virtual void disconnect() noexcept = 0;
    virtual ConnectionStatus status() const noexcept = 0;

    // ── Authentication ──
    virtual bool authenticate(const std::string& token) = 0;

    // ── Order operations ──
    virtual bool submit_order(const Order& order) = 0;
    virtual bool cancel_order(OrderId order_id) = 0;
    virtual bool modify_order(OrderId order_id, int64_t new_price, int64_t new_quantity) = 0;

    // ── Query ──
    virtual std::vector<Order> query_orders(const std::string& symbol) = 0;
    virtual std::vector<Order> query_open_orders() = 0;

    // ── Callback registration ──
    virtual void set_callback(std::shared_ptr<IBrokerCallback> callback) = 0;

    // ── Broker info ──
    virtual std::string broker_id() const = 0;
};

// ── Mock broker for testing ──
class MockBroker : public IBroker {
public:
    explicit MockBroker(const BrokerConfig& cfg = {}) : config_(cfg) {}

    ConnectionStatus connect() override {
        status_ = ConnectionStatus::kConnected;
        if (callback_) callback_->on_connected();
        return status_;
    }

    void disconnect() noexcept override {
        status_ = ConnectionStatus::kDisconnected;
        if (callback_) callback_->on_disconnected("manual disconnect");
    }

    ConnectionStatus status() const noexcept override { return status_; }

    bool authenticate(const std::string& token) override {
        if (token.empty()) return false;
        status_ = ConnectionStatus::kAuthenticated;
        return true;
    }

    bool submit_order(const Order& order) override {
        submitted_orders_.push_back(order);
        return true;
    }

    bool cancel_order(OrderId order_id) override {
        cancelled_orders_.push_back(order_id);
        return true;
    }

    bool modify_order(OrderId order_id, int64_t new_price, int64_t new_quantity) override {
        modified_orders_.push_back({order_id, new_price, new_quantity});
        return true;
    }

    std::vector<Order> query_orders(const std::string& symbol) override {
        std::vector<Order> result;
        for (const auto& o : submitted_orders_) {
            if (o.symbol == symbol) result.push_back(o);
        }
        return result;
    }

    std::vector<Order> query_open_orders() override { return submitted_orders_; }

    void set_callback(std::shared_ptr<IBrokerCallback> callback) override {
        callback_ = std::move(callback);
    }

    std::string broker_id() const override { return config_.broker_id.empty() ? "mock" : config_.broker_id; }

    // Test helpers
    const std::vector<Order>& submitted() const { return submitted_orders_; }
    const std::vector<OrderId>& cancelled() const { return cancelled_orders_; }

private:
    BrokerConfig config_;
    ConnectionStatus status_ = ConnectionStatus::kDisconnected;
    std::shared_ptr<IBrokerCallback> callback_;
    std::vector<Order> submitted_orders_;
    std::vector<OrderId> cancelled_orders_;
    std::vector<std::tuple<OrderId, int64_t, int64_t>> modified_orders_;
};

}  // namespace quant::execution