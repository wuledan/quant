// event.h — Event base class with type-safe identification
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace quant::event {

using EventTypeId = uint32_t;

// ── Compile-time event type registration macro ──
#define DEFINE_EVENT_TYPE(EventClass, id_val)           \
    static constexpr EventTypeId kEventTypeId = id_val; \
    EventTypeId event_type_id() const override { return id_val; } \
    std::string event_name() const override { return #EventClass; }

// ── Global sequence generator ──
inline uint64_t next_sequence() {
    static std::atomic<uint64_t> seq{0};
    return seq.fetch_add(1, std::memory_order_relaxed);
}

class Event {
public:
    Event()
        : timestamp_us_(0)
        , sequence_(next_sequence()) {}

    explicit Event(int64_t timestamp_us)
        : timestamp_us_(timestamp_us)
        , sequence_(next_sequence()) {}

    virtual ~Event() = default;

    // Disable copy, allow move
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = default;
    Event& operator=(Event&&) = default;

    virtual EventTypeId event_type_id() const = 0;
    virtual std::string event_name() const = 0;

    int64_t timestamp_us() const noexcept { return timestamp_us_; }
    uint64_t sequence() const noexcept { return sequence_; }

    void set_timestamp_us(int64_t ts) noexcept { timestamp_us_ = ts; }

private:
    int64_t timestamp_us_;  // event creation time (microseconds)
    uint64_t sequence_;     // globally monotonic increasing sequence
};

}  // namespace quant::event
