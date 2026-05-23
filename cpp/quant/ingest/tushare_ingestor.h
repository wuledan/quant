// tushare_ingestor.h — Tushare HTTP real-time market data ingestor
#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/storage/storage_engine.h"

namespace quant::ingest {

using infra::CoTask;

class TushareIngestor {
public:
    struct Options {
        std::string token;
        std::chrono::seconds poll_interval{60};
        std::vector<std::string> symbols;
    };

    TushareIngestor(Options opts,
                    storage::StorageEngine& engine,
                    event::EventBus& bus);
    ~TushareIngestor();

    TushareIngestor(const TushareIngestor&) = delete;
    TushareIngestor& operator=(const TushareIngestor&) = delete;

    // Coroutine: periodically poll Tushare for latest data
    CoTask<void> start();
    void stop();

    // ── Testing ──
    friend class TushareIngestorTestAccessor;

private:
    // HTTP POST to Tushare API, return raw JSON response
    std::string http_post(const std::string& body);

    // Parse Tushare daily kline response.
    // Response: {"data":{"items":[[ts,open,high,low,close,vol,amount],...]}}
    std::vector<event::KlineRow> parse_response(const std::string& json,
                                                  const std::string& symbol);

    // Convert date string "20260122" → epoch microseconds (UTC)
    static int64_t date_to_epoch_us(const std::string& date_str);

    // Convert float price to ×10000 fixed-point int32
    static int32_t price_to_fixed(double price);

    CoTask<void> poll_loop();
    CoTask<void> poll_symbol(const std::string& symbol);

    Options opts_;
    storage::StorageEngine& engine_;
    event::EventBus& bus_;
    std::atomic<bool> stopped_{false};
};

}  // namespace quant::ingest
