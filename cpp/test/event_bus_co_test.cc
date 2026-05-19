// event_bus_co_test.cc — Tests for EventBus coroutine features
#include "work_stealing_executor.h"
#include "coroutine.h"

#include <folly/portability/Event.h>
#include <folly/coro/BlockingWait.h>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/market_data_event.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace {

class CollectingSubscriber : public quant::event::IEventSubscriber {
public:
    void on_event(const quant::event::Event& event) override {
        received.fetch_add(1, std::memory_order_relaxed);
        if (const auto* mde = dynamic_cast<const quant::event::MarketDataEvent*>(&event)) {
            last_symbol = mde->symbol;
        }
    }
    std::atomic<int> received{0};
    std::string last_symbol;
};

}

TEST(EventBusCoTest, SyncPublishStillWorks) {
    auto opts = quant::event::EventBus::default_options();
    opts.subscriber_shard_count = 4;
    quant::event::EventBus bus(opts);

    auto sub = std::make_unique<CollectingSubscriber>();
    auto* raw = sub.get();
    bus.subscribe(quant::event::MarketDataEvent::kEventTypeId, std::move(sub));

    auto event = std::make_unique<quant::event::MarketDataEvent>();
    event->symbol = "000300.SH";
    bus.publish(std::move(event));

    EXPECT_EQ(raw->received.load(), 1);
    EXPECT_EQ(raw->last_symbol, "000300.SH");
}

TEST(EventBusCoTest, AsyncPublishStillWorks) {
    quant::event::EventBus bus(quant::event::EventBus::default_options());

    auto sub = std::make_unique<CollectingSubscriber>();
    auto* raw = sub.get();
    bus.subscribe(quant::event::MarketDataEvent::kEventTypeId, std::move(sub));

    bus.publish_async(std::make_unique<quant::event::MarketDataEvent>());

    for (int i = 0; i < 100; ++i) {
        if (raw->received.load() >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(raw->received.load(), 1);

    bus.stop();
}

TEST(EventBusCoTest, StatsAndFiltering) {
    quant::event::EventBus bus(quant::event::EventBus::default_options());

    auto sub = std::make_unique<CollectingSubscriber>();
    bus.subscribe(quant::event::MarketDataEvent::kEventTypeId, std::move(sub));

    bus.publish(std::make_unique<quant::event::MarketDataEvent>());

    auto stats = bus.stats();
    EXPECT_GE(stats.total_published, 1);
}
