// time_series_store.h — Core storage integrating cache + disk persistence
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "cpp/quant/storage/column_block.h"
#include "cpp/quant/storage/disk_persistence.h"
#include "cpp/quant/storage/time_series_cache.h"

namespace quant::storage {

// ── Put/Get result types ──
enum class StoreStatus : uint8_t {
    kOk = 0,
    kInvalidArgument = 1,
    kStorageFull = 2,
    kIoError = 3,
};

// ── TimeSeriesStore: unified storage layer ──
// Hot data → TimeSeriesCache, cold data → DiskPersistence
class TimeSeriesStore {
public:
    TimeSeriesStore(size_t cache_budget_mb,
                    std::filesystem::path data_dir);
    ~TimeSeriesStore();

    TimeSeriesStore(const TimeSeriesStore&) = delete;
    TimeSeriesStore& operator=(const TimeSeriesStore&) = delete;

    // Write a column block for a symbol+type
    // Hot path: writes to cache; may trigger disk flush on full cache
    StoreStatus put(std::string_view symbol, uint8_t data_type,
                    ColumnBlock block,
                    DataSource source = DataSource::kRealtimeIngest);

    // Query blocks matching field + time range
    // Tries cache first, falls back to disk
    std::vector<ColumnBlock> query(std::string_view symbol,
                                    uint8_t data_type,
                                    DataField field,
                                    TimeRange range);

    // Query all fields (returns all matching blocks)
    std::vector<ColumnBlock> query_all(std::string_view symbol,
                                        uint8_t data_type,
                                        TimeRange range);

    // Get latest data for a field (most recent block)
    ColumnBlock latest(std::string_view symbol,
                       uint8_t data_type,
                       DataField field);

    // Flush cache → disk, sync disk writes
    StoreStatus flush();

    // Close: flush and release resources
    StoreStatus close();

    // Access underlying cache/disk (for inspection/debug)
    TimeSeriesCache& cache() noexcept { return *cache_; }
    DiskPersistence& disk() noexcept { return *disk_; }

private:
    std::vector<ColumnBlock> query_disk(std::string_view symbol,
                                         uint8_t data_type,
                                         DataField field,
                                         TimeRange range);

    std::unique_ptr<TimeSeriesCache> cache_;
    std::unique_ptr<DiskPersistence> disk_;
    bool closed_ = false;
};

}  // namespace quant::storage
