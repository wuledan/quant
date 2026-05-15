// typed_event_bus.h — Type-safe wrapper around EventBus
#pragma once

#include <functional>
#include <memory>

#include "cpp/quant/event/event_bus.h"

namespace quant::event {

// ── Helper: type-erased callback subscriber for typed events ──
template<typename EventType>
class TypedCallbackSubscriber : public IEventSubscriber {
public:
    using Callback = std::function<void(const EventType&)>;

    explicit TypedCallbackSubscriber(Callback cb)
        : callback_(std::move(cb)) {}

    void on_event(const Event& event) override {
        callback_(static_cast<const EventType&>(event));
    }

private:
    Callback callback_;
};

// ── TypedEventBus template ──
template<typename EventType>
class TypedEventBus {
public:
    using TypedCallback = std::function<void(const EventType&)>;

    explicit TypedEventBus(EventBus& bus) : bus_(bus) {}

    void publish(std::unique_ptr<EventType> event) {
        bus_.publish(std::move(event));
    }

    void publish_async(std::unique_ptr<EventType> event) {
        bus_.publish_async(std::move(event));
    }

    SubscriptionId subscribe(TypedCallback callback,
                             std::unique_ptr<IEventFilter> filter = nullptr) {
        auto* sub = new TypedCallbackSubscriber<EventType>(std::move(callback));
        return bus_.subscribe(EventType::kEventTypeId,
                              std::unique_ptr<IEventSubscriber>(sub),
                              std::move(filter));
    }

    void unsubscribe(SubscriptionId id) {
        bus_.unsubscribe(id);
    }

private:
    EventBus& bus_;
};

// ── Convenience type aliases ──
using MarketDataBus  = TypedEventBus<MarketDataEvent>;
using KlineBus       = TypedEventBus<KlineEvent>;
using TradeSignalBus = TypedEventBus<TradeSignalEvent>;
using OrderReportBus = TypedEventBus<OrderReportEvent>;
using RiskAlertBus   = TypedEventBus<RiskAlertEvent>;
using FactorUpdateBus = TypedEventBus<FactorUpdateEvent>;

}  // namespace quant::event
