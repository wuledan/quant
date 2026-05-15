// order_report_event.h — Order execution report
#pragma once

#include <cstdint>
#include <string>

#include "cpp/quant/event/event.h"

namespace quant::event {

enum class OrderStatus : uint8_t {
    kPendingSubmit = 0,
    kSubmitted     = 1,
    kPartialFilled = 2,
    kFilled        = 3,
    kCancelled     = 4,
    kRejected      = 5,
    kExpired       = 6,
};

struct OrderId {
    uint64_t internal_id;  // system internal ID
    uint64_t broker_id;    // broker order ID

    auto operator<=>(const OrderId&) const = default;
};

class OrderReportEvent : public Event {
public:
    DEFINE_EVENT_TYPE(OrderReportEvent, 4);

    OrderId     order_id;
    OrderStatus status;
    int64_t     filled_quantity;
    int32_t     filled_price;    // price * 10000
    std::string reject_reason;
};

}  // namespace quant::event
