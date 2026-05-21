// ws_event_bridge.h — Bridge EventBus events to WebSocket broadcast
//
// Subscribes to all relevant EventBus event types (Kline, Factor, Order,
// Risk, TradeSignal, MarketData) and serializes them as JSON, then
// broadcasts via WebSocketServer::broadcast() to all connected clients.
//
// Architecture:
//   EventBus → WsEventBridge (via per-type SubscriberProxy) → JSON serialize
//                                                             → WebSocketServer::broadcast()
#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/event.h"
#include "cpp/quant/network/websocket_server.h"

namespace quant::network {

// ── Event type name mapping ──
// Maps EventTypeId to a human-readable channel name for WebSocket topics.
struct EventChannelMapping {
    event::EventTypeId type_id;
    std::string channel;  // e.g. "kline", "factor", "order", "risk", "signal", "market"
};

// ── WsEventBridge statistics ──
struct WsBridgeStats {
    uint64_t events_received{0};
    uint64_t events_broadcast{0};
    uint64_t serialize_errors{0};
    uint64_t broadcast_errors{0};
};

// ── WsEventBridge: EventBus → WebSocket push ──
class WsEventBridge {
public:
    // Default channel mappings for all supported event types.
    static std::vector<EventChannelMapping> default_channel_mappings();

    explicit WsEventBridge(
        event::EventBus& bus,
        WebSocketServer& ws_server,
        std::vector<EventChannelMapping> channel_mappings = default_channel_mappings()
    );

    ~WsEventBridge();

    WsEventBridge(const WsEventBridge&) = delete;
    WsEventBridge& operator=(const WsEventBridge&) = delete;

    // ── Subscribe to all configured event types ──
    // Registers per-type subscriber proxies on the EventBus.
    // Returns true if all subscriptions succeeded.
    bool start();

    // ── Unsubscribe from all event types ──
    void stop();

    // ── Handle an event from EventBus (called by SubscriberProxy) ──
    void handle_event(const event::Event& event);

    // ── Statistics ──
    WsBridgeStats stats() const noexcept;

    // ── JSON serialization (public for testing) ──
    // Serialize an Event to a JSON string suitable for WebSocket broadcast.
    // Format: {"channel":"<name>","data":{...event fields...},"ts":<timestamp_us>}
    std::string serialize_event(const event::Event& event) const;

private:
    // ── Per-type proxy subscriber ──
    // Each event type gets its own IEventSubscriber instance that forwards
    // to WsEventBridge::handle_event(). This avoids the unique_ptr ownership
    // issue of subscribing the same object to multiple event types.
    class SubscriberProxy : public event::IEventSubscriber {
    public:
        explicit SubscriberProxy(WsEventBridge& bridge) : bridge_(bridge) {}
        void on_event(const event::Event& event) override {
            bridge_.handle_event(event);
        }
    private:
        WsEventBridge& bridge_;
    };

    // ── Per-type serialization ──
    static std::string serialize_kline(const event::Event& event);
    static std::string serialize_factor(const event::Event& event);
    static std::string serialize_order(const event::Event& event);
    static std::string serialize_risk(const event::Event& event);
    static std::string serialize_signal(const event::Event& event);
    static std::string serialize_market(const event::Event& event);

    event::EventBus& bus_;
    WebSocketServer& ws_server_;

    // Channel name lookup: EventTypeId → channel string
    std::unordered_map<event::EventTypeId, std::string> channel_map_;

    // Serialization function lookup: EventTypeId → serialize function
    using SerializeFn = std::string(*)(const event::Event&);
    std::unordered_map<event::EventTypeId, SerializeFn> serialize_map_;

    // Subscription IDs for cleanup
    std::vector<event::SubscriptionId> subscription_ids_;

    // Statistics (atomic for lock-free reads)
    std::atomic<uint64_t> events_received_{0};
    std::atomic<uint64_t> events_broadcast_{0};
    std::atomic<uint64_t> serialize_errors_{0};
    std::atomic<uint64_t> broadcast_errors_{0};
};

}  // namespace quant::network