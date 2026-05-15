// event_bus.h — EventBus: publish-subscribe intra-process messaging
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "cpp/quant/event/event.h"
#include "cpp/quant/event/event_filter.h"

namespace quant::event {

using SubscriptionId = uint64_t;

// ── Subscriber interface ──
class IEventSubscriber {
public:
    virtual ~IEventSubscriber() = default;
    virtual void on_event(const Event& event) = 0;
};

// ── EventBus ──
class EventBus {
public:
    struct Options {
        size_t subscriber_shard_count = 64;
        size_t history_buffer_size = 1024;
        bool   enable_profiling = false;

        Options() = default;
        Options(size_t ssc, size_t hbs, bool ep)
            : subscriber_shard_count(ssc)
            , history_buffer_size(hbs)
            , enable_profiling(ep) {}
    };

    explicit EventBus(const Options& opts = Options{});
    ~EventBus();

    // Disable copy/move
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // ── Publish ──
    void publish(std::unique_ptr<Event> event);
    void publish_async(std::unique_ptr<Event> event);

    // ── Subscribe ──
    SubscriptionId subscribe(EventTypeId type,
                             std::unique_ptr<IEventSubscriber> subscriber,
                             std::unique_ptr<IEventFilter> filter = nullptr);

    // ── Unsubscribe ──
    void unsubscribe(SubscriptionId id);

    // ── Replay history to subscriber ──
    void replay_history(SubscriptionId id, size_t count);

    // ── Stats ──
    struct Stats {
        uint64_t total_published{0};
        uint64_t total_delivered{0};
        uint64_t avg_publish_latency_ns{0};
        uint64_t queue_depth{0};
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quant::event
