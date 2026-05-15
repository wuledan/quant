// factor_update_event.h — Factor calculation completion notification
#pragma once

#include <cstdint>

#include "cpp/quant/event/event.h"

namespace quant::event {

using FactorId = uint32_t;

class FactorUpdateEvent : public Event {
public:
    DEFINE_EVENT_TYPE(FactorUpdateEvent, 6);

    FactorId factor_id;
    int64_t  trading_day;  // YYYYMMDD format
};

}  // namespace quant::event
