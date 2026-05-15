// trade_signal_event.h — Trade signal from strategy layer
#pragma once

#include <cstdint>
#include <string>

#include "cpp/quant/event/event.h"

namespace quant::event {

enum class OrderSide : uint8_t {
    kBuy  = 0,
    kSell = 1,
};

class TradeSignalEvent : public Event {
public:
    DEFINE_EVENT_TYPE(TradeSignalEvent, 3);

    std::string strategy_id;
    std::string symbol;
    OrderSide   side;
    double      target_weight;   // target position weight [0.0, 1.0]
    double      confidence;      // signal confidence [0.0, 1.0]
};

}  // namespace quant::event
