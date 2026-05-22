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
                               ColumnBlock block,
                               DataSource source) {
    size_t idx = shard_index(symbol);
    auto& shard = *shards_[idx];

    size_t mem = block_memory(block);
    CacheKey key{std::string(symbol), type};
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    {
        std::unique_lock lock(shard.rwlock);
        auto& entry = shard.entries[key];
        entry.blocks.push_back(std::move(block));
        entry.meta.source = source;  // Set source on first append
        shard.last_access[key] = now;
        shard.memory_used += mem;
    }
    total_memory_.fetch_add(mem, std::memory_order_relaxed);

    // Auto-evict if over budget
    if (total_memory_.load(std::memory_order_relaxed) > memory_budget_) {
        evict(memory_budget_ * 80 / 100);  // evict down to 80% of budget
    }
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

    auto it = shard.entries.find(key);
    if (it == shard.entries.end()) return {};

    std::vector<ColumnBlock> result;
    for (const auto& block : it->second.blocks) {
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
        auto it = shard.entries.find(key);
        if (it != shard.entries.end()) {
            size_t entry_mem = 0;
            for (const auto& block : it->second.blocks) {
                entry_mem += block_memory(block);
            }
            shard.entries.erase(it);
            shard.last_access.erase(key);
            shard.memory_used -= entry_mem;
            total_memory_.fetch_sub(entry_mem, std::memory_order_relaxed);
        }
    }
}

void TimeSeriesCache::evict(size_t target_bytes) {
    // Collect all entries across shards with their access timestamps
    struct EntryInfo {
        CacheKey key;
        uint64_t access_time;
        size_t entry_memory;
        size_t shard_idx;
    };

    std::vector<EntryInfo> entries;

    for (size_t i = 0; i < kShardCount; ++i) {
        auto& shard = *shards_[i];
        std::shared_lock lock(shard.rwlock);
        for (const auto& [key, value] : shard.entries) {
            auto it = shard.last_access.find(key);
            uint64_t at = (it != shard.last_access.end()) ? it->second : 0;
            size_t mem = 0;
            for (const auto& block : value.blocks) {
                mem += block_memory(block);
            }
            entries.push_back(EntryInfo{key, at, mem, i});
        }
    }

    // Sort by access time ascending (LRU first)
    std::sort(entries.begin(), entries.end(),
        [](const EntryInfo& a, const EntryInfo& b) {
            return a.access_time < b.access_time;
        });

    // Evict entries until under budget
    size_t current = total_memory_.load(std::memory_order_relaxed);
    for (const auto& entry : entries) {
        if (current <= target_bytes) break;

        auto& shard = *shards_[entry.shard_idx];
        std::unique_lock lock(shard.rwlock);

        auto it = shard.entries.find(entry.key);
        if (it == shard.entries.end()) continue;

        size_t entry_mem = 0;
        for (const auto& block : it->second.blocks) {
            entry_mem += block_memory(block);
        }
        shard.entries.erase(it);
        shard.last_access.erase(entry.key);
        shard.memory_used -= entry_mem;
        current -= entry_mem;
        total_memory_.fetch_sub(entry_mem, std::memory_order_relaxed);
    }
}

CacheEntryMeta TimeSeriesCache::get_meta(std::string_view symbol,
                                          quant::event::DataType type) const {
    size_t idx = shard_index(symbol);
    auto& shard = *shards_[idx];

    CacheKey key{std::string(symbol), type};

    std::shared_lock lock(shard.rwlock);
    auto it = shard.entries.find(key);
    if (it == shard.entries.end()) {
        return CacheEntryMeta{};  // return default meta if not found
    }
    return it->second.meta;
}

void TimeSeriesCache::set_meta(std::string_view symbol,
                                quant::event::DataType type,
                                CacheEntryMeta meta) {
    size_t idx = shard_index(symbol);
    auto& shard = *shards_[idx];

    CacheKey key{std::string(symbol), type};

    std::unique_lock lock(shard.rwlock);
    auto it = shard.entries.find(key);
    if (it != shard.entries.end()) {
        it->second.meta = meta;
    }
}

}  // namespace quant::storage
