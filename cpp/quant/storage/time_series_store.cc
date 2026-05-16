// time_series_store.cc — TimeSeriesStore implementation
#include "cpp/quant/storage/time_series_store.h"

#include <algorithm>
#include <climits>

namespace quant::storage {

TimeSeriesStore::TimeSeriesStore(size_t cache_budget_mb,
                                  std::filesystem::path data_dir)
    : cache_(std::make_unique<TimeSeriesCache>(cache_budget_mb))
    , disk_(std::make_unique<DiskPersistence>(std::move(data_dir))) {}

TimeSeriesStore::~TimeSeriesStore() {
    close();
}

StoreStatus TimeSeriesStore::put(std::string_view symbol, uint8_t data_type,
                                   ColumnBlock block) {
    if (closed_) return StoreStatus::kStorageFull;
    if (symbol.empty()) return StoreStatus::kInvalidArgument;

    // Write to cache
    cache_->append(symbol,
                   static_cast<quant::event::DataType>(data_type),
                   std::move(block));

    // If cache is near full, flush oldest data to disk
    if (cache_->used_memory() > cache_->memory_budget() * 0.9) {
        // In a full implementation, we would select and evict cold entries
        // For now, we rely on the cache's implicit eviction
    }

    return StoreStatus::kOk;
}

std::vector<ColumnBlock> TimeSeriesStore::query(
    std::string_view symbol, uint8_t data_type,
    DataField field, TimeRange range) {
    // Query cache first
    auto results = cache_->query(
        symbol,
        static_cast<quant::event::DataType>(data_type),
        field, range);

    // Query disk for any data not in cache
    auto disk_results = query_disk(symbol, data_type, field, range);

    // Merge: disk results may have data outside cache's time range
    results.insert(results.end(),
                   std::make_move_iterator(disk_results.begin()),
                   std::make_move_iterator(disk_results.end()));

    return results;
}

std::vector<ColumnBlock> TimeSeriesStore::query_all(
    std::string_view symbol, uint8_t data_type, TimeRange range) {
    // Query all fields from disk
    auto segments = disk_->list_segments(symbol, data_type);

    std::vector<ColumnBlock> results;
    for (const auto& seg : segments) {
        auto blocks = disk_->read_segment(seg);
        for (auto& block : blocks) {
            if (block.max_timestamp() >= range.begin_ts &&
                block.min_timestamp() <= range.end_ts) {
                results.push_back(std::move(block));
            }
        }
    }
    return results;
}

ColumnBlock TimeSeriesStore::latest(std::string_view symbol,
                                     uint8_t data_type,
                                     DataField field) {
    // Check cache first
    auto cache_results = cache_->query(
        symbol,
        static_cast<quant::event::DataType>(data_type),
        field, TimeRange{0, INT64_MAX});

    if (!cache_results.empty()) {
        // Return the block with the latest max_timestamp
        auto it = std::max_element(
            cache_results.begin(), cache_results.end(),
            [](const ColumnBlock& a, const ColumnBlock& b) {
                return a.max_timestamp() < b.max_timestamp();
            });
        return std::move(*it);
    }

    // Fall back to disk: find newest segment
    auto segments = disk_->list_segments(symbol, data_type);
    if (segments.empty()) return ColumnBlock{};

    auto last_seg = disk_->read_segment(segments.back());
    for (auto it = last_seg.rbegin(); it != last_seg.rend(); ++it) {
        if (it->field() == field) {
            return std::move(*it);
        }
    }

    return ColumnBlock{};
}

std::vector<ColumnBlock> TimeSeriesStore::query_disk(
    std::string_view symbol, uint8_t data_type,
    DataField field, TimeRange range) {
    auto segments = disk_->list_segments(symbol, data_type);

    std::vector<ColumnBlock> results;
    for (const auto& seg : segments) {
        auto blocks = disk_->read_segment_filtered(
            seg, field, range.begin_ts, range.end_ts);
        results.insert(results.end(),
                       std::make_move_iterator(blocks.begin()),
                       std::make_move_iterator(blocks.end()));
    }
    return results;
}

StoreStatus TimeSeriesStore::flush() {
    if (closed_) return StoreStatus::kStorageFull;
    disk_->flush();
    return StoreStatus::kOk;
}

StoreStatus TimeSeriesStore::close() {
    if (closed_) return StoreStatus::kOk;
    closed_ = true;
    flush();
    return StoreStatus::kOk;
}

}  // namespace quant::storage
