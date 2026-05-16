// event_filter.h — Event filters for subscriber registration
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <span>

#include "cpp/quant/event/event.h"
#include "cpp/quant/event/events/market_data_event.h"
#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/event/events/trade_signal_event.h"

namespace quant::event {

// ── Abstract event filter ──
class IEventFilter {
public:
    virtual ~IEventFilter() = default;
    virtual bool accept(const Event& event) const = 0;
};

// ── Symbol filter: only accept events for specific symbols ──
class SymbolFilter : public IEventFilter {
public:
    explicit SymbolFilter(std::span<const std::string> symbols) {
        for (const auto& s : symbols) {
            symbols_.emplace(s);
        }
    }

    bool accept(const Event& event) const override {
        // Extract symbol via dynamic_cast for known event types
        if (const auto* mde = dynamic_cast<const MarketDataEvent*>(&event)) {
            return symbols_.count(mde->symbol) > 0;
        }
        if (const auto* ke = dynamic_cast<const KlineEvent*>(&event)) {
            return symbols_.count(ke->symbol) > 0;
        }
        if (const auto* tse = dynamic_cast<const TradeSignalEvent*>(&event)) {
            return symbols_.count(tse->symbol) > 0;
        }
        // Events without a symbol pass through
        return true;
    }

private:
    std::unordered_set<std::string> symbols_;
};

// ── Event type filter: only accept specific event types ──
class EventTypeFilter : public IEventFilter {
public:
    explicit EventTypeFilter(std::span<const EventTypeId> types)
        : types_(types.begin(), types.end()) {}

    bool accept(const Event& event) const override {
        auto id = event.event_type_id();
        return std::find(types_.begin(), types_.end(), id) != types_.end();
    }

private:
    std::vector<EventTypeId> types_;
};

}  // namespace quant::event