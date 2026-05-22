// ws_event_bridge.cc — Bridge EventBus events to WebSocket broadcast
//
// Subscribes to EventBus events and serializes them as JSON for
// WebSocket broadcast to all connected frontend clients.

#include "cpp/quant/network/ws_event_bridge.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/event/events/factor_update_event.h"
#include "cpp/quant/event/events/order_report_event.h"
#include "cpp/quant/event/events/risk_alert_event.h"
#include "cpp/quant/event/events/trade_signal_event.h"
#include "cpp/quant/event/events/market_data_event.h"

namespace quant::network {

// ────────────────────────────────────────────────────────────────
// Default channel mappings
// ────────────────────────────────────────────────────────────────

std::vector<EventChannelMapping> WsEventBridge::default_channel_mappings() {
    return {
        {event::KlineEvent::kEventTypeId,        "kline"},
        {event::FactorUpdateEvent::kEventTypeId,  "factor"},
        {event::OrderReportEvent::kEventTypeId,   "order"},
        {event::RiskAlertEvent::kEventTypeId,     "risk"},
        {event::TradeSignalEvent::kEventTypeId,   "signal"},
        {event::MarketDataEvent::kEventTypeId,    "market"},
    };
}

// ────────────────────────────────────────────────────────────────
// Construction / Destruction
// ────────────────────────────────────────────────────────────────

WsEventBridge::WsEventBridge(
    event::EventBus& bus,
    WebSocketServer& ws_server,
    std::vector<EventChannelMapping> channel_mappings
) : bus_(bus), ws_server_(ws_server) {
    for (auto& mapping : channel_mappings) {
        channel_map_[mapping.type_id] = mapping.channel;
    }

    // Register per-type serialization functions
    serialize_map_[event::KlineEvent::kEventTypeId]       = &serialize_kline;
    serialize_map_[event::FactorUpdateEvent::kEventTypeId] = &serialize_factor;
    serialize_map_[event::OrderReportEvent::kEventTypeId]  = &serialize_order;
    serialize_map_[event::RiskAlertEvent::kEventTypeId]    = &serialize_risk;
    serialize_map_[event::TradeSignalEvent::kEventTypeId]  = &serialize_signal;
    serialize_map_[event::MarketDataEvent::kEventTypeId]   = &serialize_market;
}

WsEventBridge::~WsEventBridge() {
    stop();
}

// ────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────

bool WsEventBridge::start() {
    bool all_ok = true;
    for (const auto& [type_id, channel] : channel_map_) {
        // Each event type gets its own SubscriberProxy that forwards
        // to handle_event(). This satisfies EventBus's unique_ptr ownership.
        auto sub_id = bus_.subscribe(
            type_id,
            std::make_unique<SubscriberProxy>(*this)
        );
        if (sub_id == 0) {
            all_ok = false;
        }
        subscription_ids_.push_back(sub_id);
    }
    return all_ok;
}

void WsEventBridge::stop() {
    for (auto sub_id : subscription_ids_) {
        if (sub_id != 0) {
            bus_.unsubscribe(sub_id);
        }
    }
    subscription_ids_.clear();
}

// ────────────────────────────────────────────────────────────────
// Event handling (called by SubscriberProxy)
// ────────────────────────────────────────────────────────────────

void WsEventBridge::handle_event(const event::Event& event) {
    events_received_.fetch_add(1, std::memory_order_relaxed);

    std::string json = serialize_event(event);
    if (json.empty()) {
        serialize_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    ws_server_.broadcast(json);
    events_broadcast_.fetch_add(1, std::memory_order_relaxed);
}

// ────────────────────────────────────────────────────────────────
// Statistics
// ────────────────────────────────────────────────────────────────

WsBridgeStats WsEventBridge::stats() const noexcept {
    return WsBridgeStats{
        .events_received   = events_received_.load(std::memory_order_relaxed),
        .events_broadcast  = events_broadcast_.load(std::memory_order_relaxed),
        .serialize_errors  = serialize_errors_.load(std::memory_order_relaxed),
        .broadcast_errors  = broadcast_errors_.load(std::memory_order_relaxed),
    };
}

// ────────────────────────────────────────────────────────────────
// JSON serialization — dispatch
// ────────────────────────────────────────────────────────────────

std::string WsEventBridge::serialize_event(const event::Event& event) const {
    auto type_id = event.event_type_id();

    // Find channel name
    std::string channel = "unknown";
    auto ch_it = channel_map_.find(type_id);
    if (ch_it != channel_map_.end()) {
        channel = ch_it->second;
    }

    // Find type-specific serializer
    std::string data_json;
    auto ser_it = serialize_map_.find(type_id);
    if (ser_it != serialize_map_.end()) {
        data_json = ser_it->second(event);
    } else {
        // Fallback: minimal event info
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            R"({"event_name":"%s","type_id":%u})",
            event.event_name().c_str(),
            static_cast<unsigned>(type_id));
        data_json = buf;
    }

    // Wrap in envelope: {"channel":"...","data":{...},"ts":...}
    std::string result;
    result.reserve(256 + data_json.size());
    result = R"({"channel":")" + channel + R"(","data":)" + data_json
           + R"(,"ts":)" + std::to_string(event.timestamp_us()) + "}";
    return result;
}

// ────────────────────────────────────────────────────────────────
// Per-type JSON serialization
// ────────────────────────────────────────────────────────────────

std::string WsEventBridge::serialize_kline(const event::Event& event) {
    auto& kline_evt = static_cast<const event::KlineEvent&>(event);

    // Convert fixed-point prices back to float for frontend display
    double open  = static_cast<double>(kline_evt.kline.open_price)  / 10000.0;
    double high  = static_cast<double>(kline_evt.kline.high_price)  / 10000.0;
    double low   = static_cast<double>(kline_evt.kline.low_price)   / 10000.0;
    double close = static_cast<double>(kline_evt.kline.close_price) / 10000.0;
    double vwap  = static_cast<double>(kline_evt.kline.vwap)        / 10000.0;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"symbol":"%s","kline_type":%u,"timestamp":%lld,"open":%.2f,"high":%.2f,"low":%.2f,"close":%.2f,"volume":%lld,"amount":%lld,"vwap":%.2f})",
        kline_evt.symbol.c_str(),
        static_cast<unsigned>(kline_evt.kline_type),
        static_cast<long long>(kline_evt.kline.timestamp),
        open, high, low, close,
        static_cast<long long>(kline_evt.kline.volume),
        static_cast<long long>(kline_evt.kline.amount),
        vwap);
    return buf;
}

std::string WsEventBridge::serialize_factor(const event::Event& event) {
    auto& factor_evt = static_cast<const event::FactorUpdateEvent&>(event);

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        R"({"factor_id":%u,"trading_day":%lld})",
        static_cast<unsigned>(factor_evt.factor_id),
        static_cast<long long>(factor_evt.trading_day));
    return buf;
}

std::string WsEventBridge::serialize_order(const event::Event& event) {
    auto& order_evt = static_cast<const event::OrderReportEvent&>(event);

    double filled_price = static_cast<double>(order_evt.filled_price) / 10000.0;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"internal_id":%llu,"broker_id":%llu,"status":%u,"filled_quantity":%lld,"filled_price":%.2f,"reject_reason":"%s"})",
        static_cast<unsigned long long>(order_evt.order_id.internal_id),
        static_cast<unsigned long long>(order_evt.order_id.broker_id),
        static_cast<unsigned>(order_evt.status),
        static_cast<long long>(order_evt.filled_quantity),
        filled_price,
        order_evt.reject_reason.c_str());
    return buf;
}

std::string WsEventBridge::serialize_risk(const event::Event& event) {
    auto& risk_evt = static_cast<const event::RiskAlertEvent&>(event);

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"rule_id":%u,"risk_level":%u,"severity":%u,"message":"%s"})",
        static_cast<unsigned>(risk_evt.rule_id),
        static_cast<unsigned>(risk_evt.risk_level),
        static_cast<unsigned>(risk_evt.severity),
        risk_evt.message.c_str());
    return buf;
}

std::string WsEventBridge::serialize_signal(const event::Event& event) {
    auto& sig_evt = static_cast<const event::TradeSignalEvent&>(event);

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"strategy_id":"%s","symbol":"%s","side":%u,"target_weight":%.4f,"confidence":%.4f})",
        sig_evt.strategy_id.c_str(),
        sig_evt.symbol.c_str(),
        static_cast<unsigned>(sig_evt.side),
        sig_evt.target_weight,
        sig_evt.confidence);
    return buf;
}

std::string WsEventBridge::serialize_market(const event::Event& event) {
    auto& mkt_evt = static_cast<const event::MarketDataEvent&>(event);

    double last = static_cast<double>(mkt_evt.last_price) / 10000.0;
    double bid  = static_cast<double>(mkt_evt.bid_price1) / 10000.0;
    double ask  = static_cast<double>(mkt_evt.ask_price1) / 10000.0;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"symbol":"%s","last_price":%.2f,"volume":%lld,"bid_price1":%.2f,"ask_price1":%.2f,"bid_vol1":%d,"ask_vol1":%d})",
        mkt_evt.symbol.c_str(),
        last,
        static_cast<long long>(mkt_evt.volume),
        bid, ask,
        static_cast<int>(mkt_evt.bid_vol1),
        static_cast<int>(mkt_evt.ask_vol1));
    return buf;
}

}  // namespace quant::network