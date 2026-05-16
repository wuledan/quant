// order_manager.cc — Order manager implementation
#include "cpp/quant/execution/order_manager.h"

#include <algorithm>
#include <chrono>

namespace quant::execution {

OrderManager::OrderManager() = default;
OrderManager::~OrderManager() = default;

Result<OrderId> OrderManager::create_order(const OrderRequest& req) {
    if (req.quantity <= 0) {
        return Result<OrderId>(infra::ErrorCode::InvalidArgument, "Quantity must be positive");
    }
    if (req.symbol.empty()) {
        return Result<OrderId>(infra::ErrorCode::InvalidArgument, "Symbol must not be empty");
    }

    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(mutex_);

    OrderId id = next_order_id();
    Order order;
    order.order_id = id;
    order.client_order_id = req.client_order_id;
    order.symbol = req.symbol;
    order.side = req.side;
    order.type = req.type;
    order.time_in_force = req.tif;
    order.price = req.price;
    order.stop_price = req.stop_price;
    order.quantity = req.quantity;
    order.status = OrderStatus::kPendingNew;
    order.created_at_ns = now_ns;
    order.updated_at_ns = now_ns;
    order.ext_data = req.ext_data;

    orders_.emplace(id, std::move(order));
    symbol_index_[req.symbol].push_back(id);

    return Result<OrderId>(id);
}

Result<void> OrderManager::cancel_order(OrderId order_id) {
    return apply_transition(order_id, OrderStatus::kPendingCancel);
}

Result<void> OrderManager::modify_order(const OrderModifyRequest& req) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(req.order_id);
    if (it == orders_.end()) {
        return Result<void>(infra::ErrorCode::DataNotFound, "Order not found");
    }
    if (!OrderStateMachine::can_modify(it->second.status)) {
        return Result<void>(infra::ErrorCode::InvalidArgument,
            "Order cannot be modified in current state");
    }
    auto& order = it->second;
    if (req.new_price > 0) order.price = req.new_price;
    if (req.new_quantity > 0) order.quantity = req.new_quantity;
    if (req.new_stop_price > 0) order.stop_price = req.new_stop_price;
    if (!req.ext_data.empty()) order.ext_data = req.ext_data;
    order.updated_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return Result<void>();
}

Result<void> OrderManager::on_order_accepted(OrderId order_id, std::string broker_order_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return Result<void>(infra::ErrorCode::DataNotFound, "Order not found");
    }
    it->second.broker_order_id = std::move(broker_order_id);
    it->second.status = OrderStatus::kNew;
    it->second.updated_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return Result<void>();
}

Result<void> OrderManager::on_order_rejected(OrderId order_id, std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return Result<void>(infra::ErrorCode::DataNotFound, "Order not found");
    }
    it->second.reject_reason = std::move(reason);
    it->second.status = OrderStatus::kRejected;
    it->second.updated_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return Result<void>();
}

Result<void> OrderManager::on_order_fill(OrderId order_id, int64_t fill_qty, int64_t fill_price) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return Result<void>(infra::ErrorCode::DataNotFound, "Order not found");
    }
    auto& order = it->second;
    order.filled_quantity += fill_qty;
    order.filled_amount += fill_qty * fill_price;
    order.avg_fill_price = order.filled_quantity > 0
        ? order.filled_amount / order.filled_quantity : 0;
    order.status = order.filled_quantity >= order.quantity
        ? OrderStatus::kFilled : OrderStatus::kPartialFilled;
    order.updated_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return Result<void>();
}

Result<void> OrderManager::on_order_cancelled(OrderId order_id) {
    return apply_transition(order_id, OrderStatus::kCancelled);
}

Result<void> OrderManager::on_order_expired(OrderId order_id) {
    return apply_transition(order_id, OrderStatus::kExpired);
}

Result<void> OrderManager::on_order_suspended(OrderId order_id) {
    return apply_transition(order_id, OrderStatus::kSuspended);
}

const Order* OrderManager::find_order(OrderId order_id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(order_id);
    return it != orders_.end() ? &it->second : nullptr;
}

std::vector<const Order*> OrderManager::find_orders_by_symbol(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<const Order*> result;
    auto it = symbol_index_.find(symbol);
    if (it != symbol_index_.end()) {
        for (auto id : it->second) {
            auto oit = orders_.find(id);
            if (oit != orders_.end()) result.push_back(&oit->second);
        }
    }
    return result;
}

std::vector<const Order*> OrderManager::find_orders_by_status(OrderStatus status) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<const Order*> result;
    for (const auto& [id, order] : orders_) {
        if (order.status == status) result.push_back(&order);
    }
    return result;
}

std::vector<const Order*> OrderManager::all_orders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<const Order*> result;
    result.reserve(orders_.size());
    for (const auto& [id, order] : orders_) {
        result.push_back(&order);
    }
    return result;
}

size_t OrderManager::active_order_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [id, order] : orders_) {
        if (OrderStateMachine::is_active(order.status) ||
            order.status == OrderStatus::kPendingNew ||
            order.status == OrderStatus::kPendingCancel) {
            ++count;
        }
    }
    return count;
}

Result<void> OrderManager::apply_transition(OrderId order_id, OrderStatus new_status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return Result<void>(infra::ErrorCode::DataNotFound, "Order not found");
    }
    auto result = OrderStateMachine::apply_transition(it->second.status, new_status);
    if (!result.ok()) return result;
    it->second.status = new_status;
    it->second.updated_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return Result<void>();
}

}  // namespace quant::execution
