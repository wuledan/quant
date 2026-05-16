// time_series_cache.cc — TimeSeriesCache implementation
#include "cpp/quant/storage/time_series_cache.h"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace quant::storage {

TimeSeriesCache::TimeSeriesCache(size_t memory_budget_mb)
    : memory_budget_(memory_budget_mb * 1024 * 1024) {
    for (auto& shard : shards_) {
        shard = std::make_unique<Shard>();
    }
}

void TimeSeriesCache::append(std::string_view symbol,
                               quant::event::DataType type,
                               ColumnBlock block) {
    size_t idx = shard_index(symbol);
    auto& shard = *shards_[idx];

    size_t mem = block_memory(block);
    CacheKey key{std::string(symbol), type};
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    {
        std::unique_lock lock(shard.rwlock);
        shard.columns[key].push_back(std::move(block));
        shard.last_access[key] = now;
        shard.memory_used += mem;
    }
    total_memory_.fetch_add(mem, std::memory_order_relaxed);
}

std::vector<ColumnBlock> TimeSeriesCache::query(
    std::string_view symbol,
    quant::event::DataType type,
    DataField field,
    TimeRange range) const {
    size_t idx = shard_index(symbol);
    auto& shard = *shards_[idx];

    CacheKey key{std::string(symbol), type};
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::shared_lock lock(shard.rwlock);

    // Update access timestamp
    auto access_it = shard.last_access.find(key);
    if (access_it != shard.last_access.end()) {
        access_it->second = now;
    }

    auto it = shard.columns.find(key);
    if (it == shard.columns.end()) return {};

    std::vector<ColumnBlock> result;
    for (const auto& block : it->second) {
        if (block.field() == field) {
            // Check time range overlap
            if (block.max_timestamp() >= range.begin_ts &&
                block.min_timestamp() <= range.end_ts) {
                result.push_back(block);
            }
        }
    }
    return result;
}

void TimeSeriesCache::evict(std::string_view symbol, quant::event::DataType type) {
    size_t idx = shard_index(symbol);
    auto& shard = *shards_[idx];

    CacheKey key{std::string(symbol), type};

    {
        std::unique_lock lock(shard.rwlock);
        auto it = shard.columns.find(key);
        if (it != shard.columns.end()) {
            size_t before = shard.memory_used;
            shard.columns.erase(it);
            shard.last_access.erase(key);
            size_t freed = before - shard.memory_used;
            total_memory_.fetch_sub(freed, std::memory_order_relaxed);
        }
    }
}

}  // namespace quant::storage
