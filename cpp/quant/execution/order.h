// order.h — Order data structures for execution engine
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace quant::execution {

// ── Order identifiers ──
using OrderId = uint64_t;
using ClientOrderId = uint64_t;

// ── Order side ──
enum class OrderSide : uint8_t {
    kBuy  = 0,
    kSell = 1,
};

inline std::string_view to_string(OrderSide side) {
    return side == OrderSide::kBuy ? "Buy" : "Sell";
}

// ── Order type ──
enum class OrderType : uint8_t {
    kMarket     = 0,  // 市价单
    kLimit      = 1,  // 限价单
    kStop       = 2,  // 止损单
    kStopLimit  = 3,  // 止损限价单
};

inline std::string_view to_string(OrderType type) {
    switch (type) {
        case OrderType::kMarket:    return "Market";
        case OrderType::kLimit:     return "Limit";
        case OrderType::kStop:      return "Stop";
        case OrderType::kStopLimit: return "StopLimit";
        default:                    return "Unknown";
    }
}

// ── Time-in-force ──
enum class TimeInForce : uint8_t {
    kDay     = 0,  // 当日有效
    kIOC     = 1,  // Immediate-or-Cancel
    kFOK     = 2,  // Fill-or-Kill
    kGTD     = 3,  // Good-Till-Date
    kGTC     = 4,  // Good-Till-Cancelled
};

// ── Order status (state machine states) ──
enum class OrderStatus : uint8_t {
    kPendingNew        = 0,   // Initial: awaiting submission
    kNew               = 1,   // Accepted by broker
    kPartialFilled     = 2,   // Partially filled
    kFilled            = 3,   // Fully filled
    kCancelled         = 4,   // Cancelled
    kPendingCancel     = 5,   // Cancel requested, awaiting confirmation
    kRejected          = 6,   // Rejected by broker or system
    kExpired           = 7,   // Expired (GTD/GTC)
    kSuspended         = 8,   // Suspended by exchange
};

inline std::string_view to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::kPendingNew:    return "PendingNew";
        case OrderStatus::kNew:           return "New";
        case OrderStatus::kPartialFilled: return "PartialFilled";
        case OrderStatus::kFilled:        return "Filled";
        case OrderStatus::kCancelled:     return "Cancelled";
        case OrderStatus::kPendingCancel: return "PendingCancel";
        case OrderStatus::kRejected:      return "Rejected";
        case OrderStatus::kExpired:       return "Expired";
        case OrderStatus::kSuspended:     return "Suspended";
        default:                          return "Unknown";
    }
}

// ── Order record ──
struct Order {
    OrderId         order_id       = 0;
    ClientOrderId   client_order_id = 0;
    std::string     symbol;             // stock code
    OrderSide       side             = OrderSide::kBuy;
    OrderType       type             = OrderType::kLimit;
    TimeInForce     time_in_force    = TimeInForce::kDay;
    OrderStatus     status           = OrderStatus::kPendingNew;

    int64_t         price           = 0;   // price * 10000 (fixed-point)
    int64_t         stop_price      = 0;   // for stop orders, * 10000
    int64_t         quantity        = 0;   // ordered shares
    int64_t         filled_quantity = 0;   // cum filled shares
    int64_t         filled_amount   = 0;   // cum filled amount * 10000
    int64_t         avg_fill_price  = 0;   // average fill price * 10000

    int64_t         created_at_ns   = 0;   // creation timestamp
    int64_t         updated_at_ns   = 0;   // last update timestamp

    std::string     broker_order_id;       // broker-assigned ID
    std::string     reject_reason;         // rejection reason if rejected
    std::string     ext_data;              // extra data for broker-specific fields
};

}  // namespace quant::execution
