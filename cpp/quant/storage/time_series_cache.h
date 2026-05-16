// time_series_cache.h — In-memory cache for time series data
// 64-shard design with LRU eviction
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/storage/column_block.h"

namespace quant::storage {

struct TimeRange {
    int64_t begin_ts;  // inclusive, microseconds
    int64_t end_ts;    // inclusive
};

class TimeSeriesCache {
public:
    explicit TimeSeriesCache(size_t memory_budget_mb);
    ~TimeSeriesCache() = default;

    TimeSeriesCache(const TimeSeriesCache&) = delete;
    TimeSeriesCache& operator=(const TimeSeriesCache&) = delete;

    void append(std::string_view symbol,
                quant::event::DataType type,
                ColumnBlock block);

    std::vector<ColumnBlock> query(std::string_view symbol,
                                    quant::event::DataType type,
                                    DataField field,
                                    TimeRange range) const;

    void evict(std::string_view symbol, quant::event::DataType type);

    size_t used_memory() const noexcept { return total_memory_.load(std::memory_order_relaxed); }
    size_t memory_budget() const noexcept { return memory_budget_; }
    size_t shard_count() const noexcept { return kShardCount; }

private:
    struct CacheKey {
        std::string symbol;
        quant::event::DataType type;
        bool operator==(const CacheKey& o) const {
            return symbol == o.symbol && type == o.type;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            return std::hash<std::string>()(k.symbol) ^
                   (std::hash<int>()(static_cast<int>(k.type)) << 16);
        }
    };

    struct Shard {
        mutable std::shared_mutex rwlock;
        std::unordered_map<CacheKey, std::vector<ColumnBlock>, CacheKeyHash> columns;
        std::unordered_map<CacheKey, uint64_t, CacheKeyHash> last_access;
        size_t memory_used = 0;
    };

    size_t shard_index(std::string_view symbol) const {
        return std::hash<std::string_view>()(symbol) % kShardCount;
    }

    static size_t block_memory(const ColumnBlock& block) {
        return sizeof(ColumnBlock) + block.compressed_size();
    }

    static constexpr size_t kShardCount = 64;

    std::array<std::unique_ptr<Shard>, kShardCount> shards_;
    std::atomic<size_t> total_memory_{0};
    size_t memory_budget_;
};

}  // namespace quant::storage
