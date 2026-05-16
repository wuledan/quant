// storage_engine.h — Top-level storage facade providing unified data API
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/storage/time_series_store.h"

namespace quant::storage {

// ── StorageEngine: unified data storage facade ──
// Handles kline/tick data storage with compression, caching, and persistence
class StorageEngine {
public:
    struct Options {
        size_t cache_budget_mb;
        std::string data_dir;

        Options() : cache_budget_mb(256), data_dir("./data") {}
        Options(size_t mb, std::string dir) : cache_budget_mb(mb), data_dir(std::move(dir)) {}
    };

    explicit StorageEngine(Options opts = {});
    ~StorageEngine();

    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    // ── Kline operations ──
    // Store a single kline row (converts to column blocks)
    StoreStatus store_kline(std::string_view symbol,
                            quant::event::DataType kline_type,
                            const quant::event::KlineRow& row);

    // Store a batch of kline rows
    StoreStatus store_kline_batch(std::string_view symbol,
                                   quant::event::DataType kline_type,
                                   const std::vector<quant::event::KlineRow>& rows);

    // Query kline data (decompressed) for a field over time range
    struct KlineQueryResult {
        std::vector<int64_t> timestamps;
        std::vector<double>  values;
    };

    KlineQueryResult query_kline(std::string_view symbol,
                                  quant::event::DataType kline_type,
                                  DataField field,
                                  TimeRange range);

    // ── Lifetime ──
    StoreStatus flush();
    StoreStatus close();

    // ── Access underlying store ──
    TimeSeriesStore& store() noexcept { return *store_; }

private:
    Options opts_;
    std::unique_ptr<TimeSeriesStore> store_;
    bool closed_ = false;
};

}  // namespace quant::storage
