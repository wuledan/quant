// storage_engine.cc — Top-level storage orchestrator implementation
#include "cpp/quant/storage/storage_engine.h"

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/storage/write_buffer.h"

namespace quant::storage {

StorageEngine::StorageEngine(Options opts)
    : opts_(std::move(opts))
    , cache_(std::make_unique<TimeSeriesCache>(
          TimeSeriesCache::Options{.num_shards = 16, .budget_mb = opts_.cache_budget_mb})) {}

StorageEngine::~StorageEngine() { shutdown(); }

void StorageEngine::start() {
    if (started_) return;
    started_ = true;

    // Create a TimeSeriesStore for periodic disk flush
    stores_["_all"] = std::make_unique<TimeSeriesStore>(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 1, .budget_mb = 1},
        .data_dir = opts_.data_dir,
    });
}

void StorageEngine::shutdown() {
    if (!started_) return;
    stopped_.store(true);
    started_ = false;

    stop_dirty_flush();

    if (write_buffer_) {
        write_buffer_->stop_background_flush();
        write_buffer_->flush();
    }
    for (auto& [key, store] : stores_) {
        store->close();
    }
}

// ── Sync API ──

void StorageEngine::store_kline(const std::string& symbol, uint8_t data_type,
                                const KlineRow& row) {
    if (write_buffer_) {
        write_buffer_->write(symbol, data_type, row);
    } else {
        cache_->append(symbol, data_type, row);
    }
}

void StorageEngine::store_kline_batch(const std::string& symbol, uint8_t data_type,
                                      const std::vector<KlineRow>& rows) {
    cache_->append_batch(symbol, data_type, rows);
    // Also accumulate in store for periodic or explicit disk flush
    if (!stores_.empty()) {
        stores_.begin()->second->store_kline_batch(symbol, data_type, rows);
    }
}

std::vector<KlineRow> StorageEngine::query_kline(const std::string& symbol,
                                                  uint8_t data_type,
                                                  int64_t start_ts, int64_t end_ts) {
    return cache_->query(symbol, data_type, start_ts, end_ts);
}

// ── Coroutine API ──

CoTask<void> StorageEngine::co_store_kline(const std::string& symbol,
                                           uint8_t data_type,
                                           const KlineRow& row) {
    if (write_buffer_) {
        co_await write_buffer_->co_write(symbol, data_type, row);
    } else {
        co_await cache_->co_append(symbol, data_type, row);
    }
    // Also accumulate in store for periodic disk flush
    if (!stores_.empty()) {
        co_await stores_.begin()->second->co_store_kline(symbol, data_type, row);
    }
}

// co_store_kline_batch always goes to cache directly.
// WriteBuffer::do_flush_locked calls this to avoid circular routing back to write_buffer.
CoTask<void> StorageEngine::co_store_kline_batch(const std::string& symbol,
                                                  uint8_t data_type,
                                                  const std::vector<KlineRow>& rows) {
    co_await cache_->co_append_batch(symbol, data_type, rows);
    // Also accumulate in store for periodic disk flush
    if (!stores_.empty()) {
        co_await stores_.begin()->second->co_store_kline_batch(symbol, data_type, rows);
    }
}

CoTask<std::vector<KlineRow>> StorageEngine::co_query_kline(
    const std::string& symbol, uint8_t data_type,
    int64_t start_ts, int64_t end_ts) {
    if (!stores_.empty()) {
        // Route through store for read-through (cache → disk)
        co_return co_await stores_.begin()->second->co_query_kline(
            symbol, data_type, start_ts, end_ts);
    }
    co_return co_await cache_->co_query(symbol, data_type, start_ts, end_ts);
}

DiskPersistence& StorageEngine::disk() noexcept {
    if (stores_.empty()) {
        auto store = std::make_unique<TimeSeriesStore>(TimeSeriesStore::Options{
            .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 1},
            .data_dir = opts_.data_dir,
        });
        stores_["__default__"] = std::move(store);
    }
    return stores_.begin()->second->disk();
}

void StorageEngine::set_remote_storage(RemoteStorage* rs) noexcept {
    for (auto& [key, store] : stores_) {
        store->set_remote_storage(rs);
    }
}

void StorageEngine::flush_all() {
    for (auto& [key, store] : stores_) {
        infra::blockingWait(store->co_flush());
    }
}

void StorageEngine::set_write_buffer(std::unique_ptr<WriteBuffer> wb) {
    write_buffer_ = std::move(wb);
}

WriteBuffer* StorageEngine::write_buffer() noexcept {
    return write_buffer_.get();
}

void StorageEngine::close() {
    shutdown();
}

// ── Dirty flush ──

void StorageEngine::start_dirty_flush(folly::Executor* executor) {
    flush_scope_.add(
        folly::coro::co_withExecutor(executor, dirty_flush_loop()));
}

void StorageEngine::stop_dirty_flush() {
    stopped_.store(true);
}

folly::coro::Task<void> StorageEngine::dirty_flush_loop() {
    while (!stopped_.load(std::memory_order_relaxed)) {
        co_await infra::sleep(std::chrono::seconds(30));
        if (stopped_.load(std::memory_order_relaxed)) break;
        for (auto& [key, store] : stores_) {
            co_await store->co_flush();
        }
    }
}

}  // namespace quant::storage