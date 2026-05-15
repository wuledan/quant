// market_data_event.h — Market data tick event
#pragma once

#include <cstdint>
#include <string>

#include "cpp/quant/event/event.h"

namespace quant::event {

class MarketDataEvent : public Event {
public:
    DEFINE_EVENT_TYPE(MarketDataEvent, 1);

    std::string symbol;
    int32_t last_price;    // price * 10000 (fixed-point)
    int64_t volume;
    int32_t bid_price1;
    int32_t ask_price1;
    int32_t bid_vol1;
    int32_t ask_vol1;
};

}  // namespace quant::event
