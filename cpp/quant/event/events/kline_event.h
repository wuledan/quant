// kline_event.h — Kline data event
#pragma once

#include <cstdint>
#include <string>

#include "cpp/quant/event/event.h"

namespace quant::event {

// ── Kline row structure (packed, matches storage engine) ──
#pragma pack(push, 1)
struct KlineRow {
    int64_t timestamp;      // microseconds
    int32_t open_price;     // price * 10000
    int32_t high_price;
    int32_t low_price;
    int32_t close_price;
    int64_t volume;
    int64_t amount;
    int32_t vwap;            // price * 10000 (consistent with OHLC)
    int32_t _padding = 0;
};
static_assert(sizeof(KlineRow) == 48);
#pragma pack(pop)

enum class DataType : uint8_t {
    kKline1Min  = 0,
    kKline5Min  = 1,
    kKline15Min = 2,
    kKline30Min = 3,
    kKline60Min = 4,
    kKlineDay   = 5,
    kTick       = 6,
};

class KlineEvent : public Event {
public:
    DEFINE_EVENT_TYPE(KlineEvent, 2);

    std::string symbol;
    DataType    kline_type;
    KlineRow    kline;
};

}  // namespace quant::event
