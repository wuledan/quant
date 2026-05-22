// time_series_cache.cc — Sharded in-memory time-series cache implementation
#include "cpp/quant/storage/time_series_cache.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include "cpp/quant/infra/coroutine.h"

namespace quant::storage {

TimeSeriesCache::TimeSeriesCache(Options opts)
    : opts_(std::move(opts)) {
    shards_.reserve(opts_.num_shards);
    for (size_t i = 0; i < opts_.num_shards; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }
}

size_t TimeSeriesCache::shard_index(std::string_view symbol, uint8_t data_type) const {
    size_t h = std::hash<std::string_view>()(symbol);
    h ^= static_cast<size_t>(data_type) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h % shards_.size();
}

// ── Coroutine API ──

CoTask<void> TimeSeriesCache::co_append(
    std::string_view symbol, uint8_t data_type, KlineRow row,
    DataSource source) {
    auto idx = shard_index(symbol, data_type);
    auto& shard = *shards_[idx];
    auto lock = co_await shard.rwlock.co_scoped_lock();

    ShardKey key{std::string(symbol), data_type};
    auto& entry = shard.entries[key];

    // Insert maintaining sort by ts
    auto it = std::lower_bound(entry.rows.begin(), entry.rows.end(), row.timestamp,
                               [](const KlineRow& r, int64_t ts) {
                                   return r.timestamp < ts;
                               });
    entry.rows.insert(it, std::move(row));
    entry.meta.last_access_ts = std::max(entry.meta.last_access_ts, row.timestamp);
    entry.meta.source = source;

    // Track memory: one more row of 48 bytes
    entry.meta.approx_bytes = entry.rows.size() * sizeof(KlineRow) + key.symbol.size();
    total_memory_.fetch_add(sizeof(KlineRow), std::memory_order_relaxed);

    evict_if_needed(shard);
    co_return;
}

CoTask<void> TimeSeriesCache::co_append_batch(
    std::string_view symbol, uint8_t data_type, std::vector<KlineRow> rows,
    DataSource source) {
    if (rows.empty()) co_return;

    auto idx = shard_index(symbol, data_type);
    auto& shard = *shards_[idx];
    auto lock = co_await shard.rwlock.co_scoped_lock();

    ShardKey key{std::string(symbol), data_type};
    auto& entry = shard.entries[key];

    int64_t max_ts = 0;
    for (auto& r : rows) {
        max_ts = std::max(max_ts, r.timestamp);
    }

    // Merge sorted: existing rows + new rows
    auto& existing = entry.rows;
    existing.reserve(existing.size() + rows.size());
    for (auto& r : rows) {
        auto it = std::lower_bound(existing.begin(), existing.end(), r.timestamp,
                                   [](const KlineRow& row, int64_t ts) {
                                       return row.timestamp < ts;
                                   });
        existing.insert(it, std::move(r));
    }
    entry.meta.last_access_ts = std::max(entry.meta.last_access_ts, max_ts);
    entry.meta.source = source;

    // Track memory: added rows * sizeof(KlineRow)
    size_t added_bytes = rows.size() * sizeof(KlineRow);
    entry.meta.approx_bytes = existing.size() * sizeof(KlineRow) + key.symbol.size();
    total_memory_.fetch_add(added_bytes, std::memory_order_relaxed);

    evict_if_needed(shard);
    co_return;
}

CoTask<std::vector<KlineRow>> TimeSeriesCache::co_query(
    std::string_view symbol, uint8_t data_type,
    int64_t start_ts, int64_t end_ts) {
    auto idx = shard_index(symbol, data_type);
    auto& shard = *shards_[idx];
    auto lock = co_await shard.rwlock.co_scoped_shared_lock();

    ShardKey key{std::string(symbol), data_type};
    auto it = shard.entries.find(key);
    if (it == shard.entries.end()) co_return {};

    // Update access timestamp for eviction priority
    it->second.meta.last_access_ts = std::max(it->second.meta.last_access_ts,
                                               std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch()).count());

    const auto& rows = it->second.rows;
    if (rows.empty()) co_return {};

    // Binary search for [start_ts, end_ts]
    auto lo = std::lower_bound(rows.begin(), rows.end(), start_ts,
                               [](const KlineRow& r, int64_t ts) {
                                   return r.timestamp < ts;
                               });
    auto hi = std::upper_bound(lo, rows.end(), end_ts,
                               [](int64_t ts, const KlineRow& r) {
                                   return ts < r.timestamp;
                               });

    std::vector<KlineRow> result(lo, hi);
    co_return result;
}

// ── Synchronous API ──
//
// Uses try_lock/try_lock_shared directly on AffinitySharedMutex to avoid
// blockingWait which interacts poorly with coroutine-specific mutexes.

void TimeSeriesCache::append(std::string_view symbol, uint8_t data_type, KlineRow row) {
    auto idx = shard_index(symbol, data_type);
    auto& shard = *shards_[idx];

    // Spin on try_lock instead of blockingWait(co_append) since
    // AffinitySharedMutex is designed for coroutine use.
    while (!shard.rwlock.try_lock()) {
        std::this_thread::yield();
    }

    ShardKey key{std::string(symbol), data_type};
    auto& entry = shard.entries[key];

    auto it = std::lower_bound(entry.rows.begin(), entry.rows.end(), row.timestamp,
                               [](const KlineRow& r, int64_t ts) {
                                   return r.timestamp < ts;
                               });
    entry.rows.insert(it, std::move(row));
    entry.meta.last_access_ts = std::max(entry.meta.last_access_ts, row.timestamp);

    // Track memory and evict if over budget
    entry.meta.approx_bytes = entry.rows.size() * sizeof(KlineRow) + key.symbol.size();
    total_memory_.fetch_add(sizeof(KlineRow), std::memory_order_relaxed);
    evict_if_needed(shard);

    shard.rwlock.unlock();
}

void TimeSeriesCache::append_batch(std::string_view symbol, uint8_t data_type,
                                   std::vector<KlineRow> rows) {
    if (rows.empty()) return;

    auto idx = shard_index(symbol, data_type);
    auto& shard = *shards_[idx];

    while (!shard.rwlock.try_lock()) {
        std::this_thread::yield();
    }

    ShardKey key{std::string(symbol), data_type};
    auto& entry = shard.entries[key];

    int64_t max_ts = 0;
    for (auto& r : rows) {
        max_ts = std::max(max_ts, r.timestamp);
    }

    auto& existing = entry.rows;
    existing.reserve(existing.size() + rows.size());
    for (auto& r : rows) {
        auto it = std::lower_bound(existing.begin(), existing.end(), r.timestamp,
                                   [](const KlineRow& row, int64_t ts) {
                                       return row.timestamp < ts;
                                   });
        existing.insert(it, std::move(r));
    }
    entry.meta.last_access_ts = std::max(entry.meta.last_access_ts, max_ts);

    // Track memory and evict if over budget
    size_t added_bytes = rows.size() * sizeof(KlineRow);
    entry.meta.approx_bytes = existing.size() * sizeof(KlineRow) + key.symbol.size();
    total_memory_.fetch_add(added_bytes, std::memory_order_relaxed);
    evict_if_needed(shard);

    shard.rwlock.unlock();
}

std::vector<KlineRow> TimeSeriesCache::query(std::string_view symbol, uint8_t data_type,
                                             int64_t start_ts, int64_t end_ts) {
    auto idx = shard_index(symbol, data_type);
    auto& shard = *shards_[idx];

    while (!shard.rwlock.try_lock_shared()) {
        std::this_thread::yield();
    }

    ShardKey key{std::string(symbol), data_type};
    auto it = shard.entries.find(key);
    if (it == shard.entries.end()) {
        shard.rwlock.unlock_shared();
        return {};
    }

    // Update access timestamp for eviction priority
    it->second.meta.last_access_ts = std::max(it->second.meta.last_access_ts,
                                               std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch()).count());

    const auto& rows = it->second.rows;
    std::vector<KlineRow> result;

    if (!rows.empty()) {
        auto lo = std::lower_bound(rows.begin(), rows.end(), start_ts,
                                   [](const KlineRow& r, int64_t ts) {
                                       return r.timestamp < ts;
                                   });
        auto hi = std::upper_bound(lo, rows.end(), end_ts,
                                   [](int64_t ts, const KlineRow& r) {
                                       return ts < r.timestamp;
                                   });
        result.assign(lo, hi);
    }

    shard.rwlock.unlock_shared();
    return result;
}

// ── Eviction ──

void TimeSeriesCache::evict_if_needed(Shard& shard) {
    size_t current = total_memory_.load(std::memory_order_relaxed);
    size_t budget = opts_.budget();
    if (current <= budget) return;

    size_t target = static_cast<size_t>(budget * 0.8);

    // Collect candidate entries from this shard, sorted by last_access_ts (oldest first)
    std::vector<std::pair<int64_t /* last_access */, ShardKey>> candidates;
    candidates.reserve(shard.entries.size());
    for (const auto& [key, entry] : shard.entries) {
        candidates.emplace_back(entry.meta.last_access_ts, key);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Evict oldest entries until under target
    for (const auto& [access_ts, key] : candidates) {
        if (total_memory_.load(std::memory_order_relaxed) <= target) break;
        (void)access_ts;

        auto it = shard.entries.find(key);
        if (it == shard.entries.end()) continue;

        size_t freed = it->second.meta.approx_bytes;
        shard.entries.erase(it);
        total_memory_.fetch_sub(freed, std::memory_order_relaxed);
    }
}

}  // namespace quant::storage