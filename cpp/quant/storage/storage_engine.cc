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
}

void StorageEngine::shutdown() {
    if (!started_) return;
    started_ = false;
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
    cache_->append(symbol, data_type, row);
}

void StorageEngine::store_kline_batch(const std::string& symbol, uint8_t data_type,
                                      const std::vector<KlineRow>& rows) {
    cache_->append_batch(symbol, data_type, rows);
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
    co_await cache_->co_append(symbol, data_type, row);
}

CoTask<void> StorageEngine::co_store_kline_batch(const std::string& symbol,
                                                  uint8_t data_type,
                                                  const std::vector<KlineRow>& rows) {
    co_await cache_->co_append_batch(symbol, data_type, rows);
}

CoTask<std::vector<KlineRow>> StorageEngine::co_query_kline(
    const std::string& symbol, uint8_t data_type,
    int64_t start_ts, int64_t end_ts) {
    co_return co_await cache_->co_query(symbol, data_type, start_ts, end_ts);
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

}  // namespace quant::storage