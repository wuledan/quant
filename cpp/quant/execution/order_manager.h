// order_manager.h — Order manager with order lifecycle and in-memory index
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/execution/order.h"
#include "cpp/quant/execution/order_state_machine.h"
#include "cpp/quant/infra/error_codes.h"

namespace quant::execution {

using infra::Result;

// ── Order creation request ──
struct OrderRequest {
    ClientOrderId  client_order_id = 0;
    std::string    symbol;
    OrderSide      side    = OrderSide::kBuy;
    OrderType      type    = OrderType::kLimit;
    TimeInForce    tif     = TimeInForce::kDay;
    int64_t        price   = 0;
    int64_t        stop_price = 0;
    int64_t        quantity = 0;
    std::string    ext_data;
};

// ── Order modification request ──
struct OrderModifyRequest {
    OrderId   order_id;
    int64_t   new_price    = 0;
    int64_t   new_quantity = 0;
    int64_t   new_stop_price = 0;
    std::string ext_data;
};

// ── Order fill report ──
struct FillReport {
    OrderId order_id;
    int64_t fill_quantity = 0;
    int64_t fill_price    = 0;  // * 10000
    int64_t fill_time_ns  = 0;
};

class OrderManager {
public:
    OrderManager();
    ~OrderManager();

    // Disable copy
    OrderManager(const OrderManager&) = delete;
    OrderManager& operator=(const OrderManager&) = delete;

    // ── Order operations ──
    Result<OrderId> create_order(const OrderRequest& req);
    Result<void>    cancel_order(OrderId order_id);
    Result<void>    modify_order(const OrderModifyRequest& req);

    // ── Order state updates (from broker/exchange) ──
    Result<void> on_order_accepted(OrderId order_id, std::string broker_order_id);
    Result<void> on_order_rejected(OrderId order_id, std::string reason);
    Result<void> on_order_fill(OrderId order_id, int64_t fill_qty, int64_t fill_price);
    Result<void> on_order_cancelled(OrderId order_id);
    Result<void> on_order_expired(OrderId order_id);
    Result<void> on_order_suspended(OrderId order_id);

    // ── Query ──
    const Order* find_order(OrderId order_id) const noexcept;
    std::vector<const Order*> find_orders_by_symbol(const std::string& symbol) const;
    std::vector<const Order*> find_orders_by_status(OrderStatus status) const;
    std::vector<const Order*> all_orders() const;

    // ── Stats ──
    size_t total_order_count() const noexcept { return orders_.size(); }
    size_t active_order_count() const noexcept;

private:
    OrderId next_order_id() noexcept {
        return seq_.fetch_add(1, std::memory_order_relaxed);
    }

    Result<void> apply_transition(OrderId order_id, OrderStatus new_status);

    std::atomic<OrderId> seq_{1};
    mutable std::mutex mutex_;

    // Primary index: order_id -> Order
    std::unordered_map<OrderId, Order> orders_;

    // Secondary index: symbol -> order_ids
    std::unordered_map<std::string, std::vector<OrderId>> symbol_index_;
};

}  // namespace quant::execution
