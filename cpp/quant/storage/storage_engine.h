// storage_engine.h — Top-level storage orchestrator
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/storage/time_series_cache.h"
#include "cpp/quant/storage/time_series_store.h"

namespace quant::storage {

using infra::CoTask;

class WriteBuffer;

class StorageEngine {
public:
    struct Options {
        size_t cache_budget_mb = 256;
        std::filesystem::path data_dir;
    };

    explicit StorageEngine(Options opts);
    ~StorageEngine();

    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    void start();
    void shutdown();

    // ── Sync API ──
    void store_kline(const std::string& symbol, uint8_t data_type,
                     const KlineRow& row);
    void store_kline_batch(const std::string& symbol, uint8_t data_type,
                           const std::vector<KlineRow>& rows);
    std::vector<KlineRow> query_kline(const std::string& symbol, uint8_t data_type,
                                      int64_t start_ts, int64_t end_ts);

    // ── Coroutine API ──
    CoTask<void> co_store_kline(const std::string& symbol, uint8_t data_type,
                                const KlineRow& row);
    CoTask<void> co_store_kline_batch(const std::string& symbol, uint8_t data_type,
                                      const std::vector<KlineRow>& rows);
    CoTask<std::vector<KlineRow>> co_query_kline(const std::string& symbol,
                                                  uint8_t data_type,
                                                  int64_t start_ts, int64_t end_ts);

    // ── Access ──
    TimeSeriesCache& cache() noexcept { return *cache_; }
    void set_write_buffer(std::unique_ptr<WriteBuffer> wb);
    WriteBuffer* write_buffer() noexcept;
    void close();

private:
    Options opts_;
    std::unique_ptr<TimeSeriesCache> cache_;
    std::unique_ptr<WriteBuffer> write_buffer_;
    std::unordered_map<std::string, std::unique_ptr<TimeSeriesStore>> stores_;
    bool started_ = false;
};

}  // namespace quant::storage