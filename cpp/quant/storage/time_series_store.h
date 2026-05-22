// time_series_store.h — Per-symbol kline storage (cache + disk)
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/storage/disk_persistence.h"
#include "cpp/quant/storage/time_series_cache.h"

namespace quant::storage {

using infra::CoTask;

enum class StoreStatus : uint8_t {
    kOk = 0,
    kInvalidArgument = 1,
    kStorageFull = 2,
    kIoError = 3,
};

// TimeSeriesStore: hot data → TimeSeriesCache, cold → DiskPersistence
class TimeSeriesStore {
public:
    struct Options {
        TimeSeriesCache::Options cache_opts;
        std::filesystem::path data_dir;
    };

    explicit TimeSeriesStore(Options opts);
    ~TimeSeriesStore();

    TimeSeriesStore(const TimeSeriesStore&) = delete;
    TimeSeriesStore& operator=(const TimeSeriesStore&) = delete;

    // ── Sync API ──
    StoreStatus store_kline(const std::string& symbol, uint8_t data_type,
                            const KlineRow& row);
    StoreStatus store_kline_batch(const std::string& symbol, uint8_t data_type,
                                  const std::vector<KlineRow>& rows);
    std::vector<KlineRow> query_kline(const std::string& symbol, uint8_t data_type,
                                      int64_t start_ts, int64_t end_ts);

    // ── Coroutine API ──
    CoTask<StoreStatus> co_store_kline(const std::string& symbol, uint8_t data_type,
                                       const KlineRow& row);
    CoTask<StoreStatus> co_store_kline_batch(const std::string& symbol, uint8_t data_type,
                                             const std::vector<KlineRow>& rows);
    CoTask<std::vector<KlineRow>> co_query_kline(const std::string& symbol, uint8_t data_type,
                                                  int64_t start_ts, int64_t end_ts);

    // ── Access ──
    TimeSeriesCache& cache() noexcept { return *cache_; }
    DiskPersistence& disk() noexcept { return *disk_; }

    StoreStatus flush();
    StoreStatus close();

private:
    Options opts_;
    std::unique_ptr<TimeSeriesCache> cache_;
    std::unique_ptr<DiskPersistence> disk_;
    bool closed_ = false;
};

}  // namespace quant::storage