// trade_signal_event.h — Trade signal from strategy layer
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

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
    double      price{0.0};      // signal price
    int         quantity{0};     // suggested trade quantity
    std::unordered_map<std::string, double> factor_values; // factor snapshots
};

}  // namespace quant::event
