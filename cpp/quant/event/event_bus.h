// event_bus.h — EventBus: publish-subscribe intra-process messaging (coroutine-aware)
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "coroutine.h"
#include "event.h"
#include "event_filter.h"

namespace quant::event {

using SubscriptionId = uint64_t;

// ── Subscriber interface ──
class IEventSubscriber {
public:
    virtual ~IEventSubscriber() = default;
    virtual void on_event(const Event& event) = 0;
};

// ── Coroutine subscriber interface (async callback) ──
class ICoEventSubscriber {
public:
    virtual ~ICoEventSubscriber() = default;
    virtual quant::infra::CoTask<void> on_event_async(const Event& event) = 0;
};

// ── EventBus ──
class EventBus {
public:
    struct Options {
        size_t subscriber_shard_count = 64;
        size_t history_buffer_size = 1024;
        bool   enable_profiling = false;
    };

    static Options default_options() { return Options{}; }

    explicit EventBus(const Options& opts);
    ~EventBus();

    // Disable copy/move
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // ── Synchronous publish ──
    void publish(std::unique_ptr<Event> event);

    // ── Async publish (thread-based, legacy) ──
    void publish_async(std::unique_ptr<Event> event);

    // ── Coroutine publish ──
    // Enqueues and signals a coroutine dispatcher. Requires the event bus
    // to have been started with start_async().
    quant::infra::CoTask<void> co_publish(std::unique_ptr<Event> event);

    // ── Subscribe ──
    SubscriptionId subscribe(EventTypeId type,
                             std::unique_ptr<IEventSubscriber> subscriber,
                             std::unique_ptr<IEventFilter> filter = nullptr);

    // ── Unsubscribe ──
    void unsubscribe(SubscriptionId id);

    // ── Replay history to subscriber ──
    void replay_history(SubscriptionId id, size_t count);

    // ── Worker lifecycle ──
    void start();
    void stop();

    // ── Coroutine-based dispatch loop ──
    // Runs the dispatch loop on the given executor. Must be called before
    // co_publish will deliver events.
    quant::infra::CoTask<void>
    start_async(quant::infra::WorkStealingExecutor& executor);

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
