// time_series_cache.h — Sharded in-memory time-series cache
//
// Stores recent KlineRow entries for fast point queries and range scans.
// Sharded by (symbol, data_type) hash for concurrent access.
// Each shard protected by AffinitySharedMutex (thread-affine coroutine RW lock).
//
// Coroutine API: co_append, co_append_batch, co_query (preferred)
// Sync API:      append, append_batch, query (blockingWait wrappers)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <memory>

#include "cpp/quant/infra/affinity_shared_mutex.h"
#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/event/events/kline_event.h"

namespace quant::storage {

using infra::CoTask;
using KlineRow = event::KlineRow;

// How data entered the cache — used for eviction priority
enum class DataSource : uint8_t {
    kRealtimeIngest = 0,   // live market data (highest priority, evict last)
    kBatchLoad = 1,        // loaded from disk segment
    kRemoteLoad = 2,       // loaded from remote (MinIO/Parquet)
    kUnknown = 255,
};

// Per-entry metadata for eviction decisions
struct CacheEntryMeta {
    DataSource source = DataSource::kUnknown;
    int64_t last_access_ts = 0;    // monotonic timestamp of last read
    size_t approx_bytes = 0;       // approximate memory footprint
};

class TimeSeriesCache {
public:
    struct Options {
        size_t num_shards = 16;
        size_t budget_mb = 256;
    };

    explicit TimeSeriesCache(Options opts);

    // ── Coroutine API (preferred) ──

    CoTask<void> co_append(std::string_view symbol, uint8_t data_type, KlineRow row);
    CoTask<void> co_append_batch(std::string_view symbol, uint8_t data_type,
                                 std::vector<KlineRow> rows);
    CoTask<std::vector<KlineRow>> co_query(std::string_view symbol, uint8_t data_type,
                                           int64_t start_ts, int64_t end_ts);

    // ── Synchronous API (backward compatible) ──

    void append(std::string_view symbol, uint8_t data_type, KlineRow row);
    void append_batch(std::string_view symbol, uint8_t data_type,
                      std::vector<KlineRow> rows);
    std::vector<KlineRow> query(std::string_view symbol, uint8_t data_type,
                                int64_t start_ts, int64_t end_ts);

private:
    struct ShardKey {
        std::string symbol;
        uint8_t data_type;

        bool operator==(const ShardKey& o) const {
            return symbol == o.symbol && data_type == o.data_type;
        }
    };

    struct ShardKeyHash {
        size_t operator()(const ShardKey& k) const {
            return std::hash<std::string>()(k.symbol) ^
                   (static_cast<size_t>(k.data_type) << 17);
        }
    };

    struct ShardEntry {
        std::vector<KlineRow> rows;     // sorted by ts
        CacheEntryMeta meta;
    };

    // Shard: (symbol, data_type) → sorted KlineRow list
    // AffinitySharedMutex is non-movable, so Shard is non-movable
    struct Shard {
        std::unordered_map<ShardKey, ShardEntry, ShardKeyHash> entries;
        infra::AffinitySharedMutex rwlock;
    };

    size_t shard_index(std::string_view symbol, uint8_t data_type) const;

    Options opts_;
    // unique_ptr because Shard (containing AffinitySharedMutex) is non-movable
    std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace quant::storage