// event_bus_test.cc — Tests for EventBus with MPSC async queue
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/market_data_event.h"
#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/event/events/trade_signal_event.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace quant::event {
namespace {

// ── Test subscriber that collects events ──
class CollectingSubscriber : public IEventSubscriber {
public:
    void on_event(const Event& event) override {
        received.fetch_add(1, std::memory_order_relaxed);
        if (const auto* mde = dynamic_cast<const MarketDataEvent*>(&event)) {
            last_symbol = mde->symbol;
        }
    }

    std::atomic<int> received{0};
    std::string last_symbol;
};

TEST(EventBusTest, SyncPublishSubscribe) {
    auto opts = EventBus::default_options();
    opts.subscriber_shard_count = 4;
    EventBus bus(opts);

    auto sub = std::make_unique<CollectingSubscriber>();
    auto* raw = sub.get();
    bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub));

    auto event = std::make_unique<MarketDataEvent>();
    event->symbol = "000300.SH";
    bus.publish(std::move(event));

    EXPECT_EQ(raw->received.load(), 1);
    EXPECT_EQ(raw->last_symbol, "000300.SH");
}

TEST(EventBusTest, SyncPublishMultipleSubscribers) {
    EventBus bus(EventBus::default_options());

    auto sub1 = std::make_unique<CollectingSubscriber>();
    auto sub2 = std::make_unique<CollectingSubscriber>();
    auto* raw1 = sub1.get();
    auto* raw2 = sub2.get();

    bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub1));
    bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub2));

    bus.publish(std::make_unique<MarketDataEvent>());

    EXPECT_EQ(raw1->received.load(), 1);
    EXPECT_EQ(raw2->received.load(), 1);
}

TEST(EventBusTest, SubscribeUnsubscribe) {
    EventBus bus(EventBus::default_options());

    auto sub = std::make_unique<CollectingSubscriber>();
    auto id = bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub));

    bus.publish(std::make_unique<MarketDataEvent>());

    bus.unsubscribe(id);
    // After unsubscribe, subscriber is destroyed; verify no crash
    auto stats = bus.stats();
    EXPECT_GE(stats.total_delivered, 1);

    bus.publish(std::make_unique<MarketDataEvent>());
}

TEST(EventBusTest, TypeFiltering) {
    EventBus bus(EventBus::default_options());

    auto sub = std::make_unique<CollectingSubscriber>();
    auto* raw = sub.get();
    bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub));

    // KlineEvent should not be delivered to MarketDataEvent subscriber
    bus.publish(std::make_unique<KlineEvent>());
    EXPECT_EQ(raw->received.load(), 0);

    bus.publish(std::make_unique<MarketDataEvent>());
    EXPECT_EQ(raw->received.load(), 1);
}

TEST(EventBusTest, AsyncPublishSubscribe) {
    auto opts = EventBus::default_options();
    opts.subscriber_shard_count = 4;
    EventBus bus(opts);

    auto sub = std::make_unique<CollectingSubscriber>();
    auto* raw = sub.get();
    bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub));

    bus.publish_async(std::make_unique<MarketDataEvent>());
    bus.publish_async(std::make_unique<MarketDataEvent>());

    // Wait for async delivery
    for (int i = 0; i < 100; ++i) {
        if (raw->received.load() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(raw->received.load(), 2);
}

TEST(EventBusTest, AsyncWorkerLazyStart) {
    // Verify worker starts on first publish_async call
    EventBus bus(EventBus::default_options());
    auto sub = std::make_unique<CollectingSubscriber>();
    auto* raw = sub.get();
    bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub));

    bus.publish_async(std::make_unique<MarketDataEvent>());

    for (int i = 0; i < 100; ++i) {
        if (raw->received.load() >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(raw->received.load(), 1);
}

TEST(EventBusTest, StatsTracking) {
    EventBus bus(EventBus::default_options());

    auto sub = std::make_unique<CollectingSubscriber>();
    bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub));

    bus.publish(std::make_unique<MarketDataEvent>());
    bus.publish(std::make_unique<KlineEvent>());

    auto stats = bus.stats();
    EXPECT_GE(stats.total_published, 2);
    EXPECT_GE(stats.total_delivered, 1);  // only MarketDataEvent matches
}

TEST(EventBusTest, EventFilterAccept) {
    EventBus bus(EventBus::default_options());

    auto sub = std::make_unique<CollectingSubscriber>();
    auto* raw = sub.get();

    // Filter that accepts only "000300.SH"
    std::vector<std::string> symbols = {"000300.SH"};
    auto filter = std::make_unique<SymbolFilter>(symbols);

    bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub), std::move(filter));

    auto event1 = std::make_unique<MarketDataEvent>();
    event1->symbol = "000300.SH";
    bus.publish(std::move(event1));
    EXPECT_EQ(raw->received.load(), 1);

    auto event2 = std::make_unique<MarketDataEvent>();
    event2->symbol = "000688.SH";
    bus.publish(std::move(event2));
    EXPECT_EQ(raw->received.load(), 1);  // filtered out
}

TEST(EventBusTest, MultipleEventTypes) {
    EventBus bus(EventBus::default_options());

    auto md_sub = std::make_unique<CollectingSubscriber>();
    auto kline_sub = std::make_unique<CollectingSubscriber>();
    auto* md_raw = md_sub.get();
    auto* kline_raw = kline_sub.get();

    bus.subscribe(MarketDataEvent::kEventTypeId, std::move(md_sub));
    bus.subscribe(KlineEvent::kEventTypeId, std::move(kline_sub));

    bus.publish(std::make_unique<MarketDataEvent>());
    bus.publish(std::make_unique<KlineEvent>());

    EXPECT_EQ(md_raw->received.load(), 1);
    EXPECT_EQ(kline_raw->received.load(), 1);
}

TEST(EventBusTest, ReplayHistoryNotImplemented) {
    EventBus bus(EventBus::default_options());
    auto sub = std::make_unique<CollectingSubscriber>();
    auto id = bus.subscribe(MarketDataEvent::kEventTypeId, std::move(sub));
    // Should not crash
    bus.replay_history(id, 10);
}

}  // namespace
}  // namespace quant::event
