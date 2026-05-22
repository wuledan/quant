// time_series_store.cc — Per-symbol kline storage (cache + disk)
#include "cpp/quant/storage/time_series_store.h"

namespace quant::storage {

TimeSeriesStore::TimeSeriesStore(Options opts)
    : opts_(std::move(opts))
    , cache_(std::make_unique<TimeSeriesCache>(opts_.cache_opts))
    , disk_(std::make_unique<DiskPersistence>(opts_.data_dir)) {}

TimeSeriesStore::~TimeSeriesStore() { close(); }

// ── Sync API ──

StoreStatus TimeSeriesStore::store_kline(const std::string& symbol,
                                          uint8_t data_type,
                                          const KlineRow& row) {
    cache_->append(symbol, data_type, row);
    return StoreStatus::kOk;
}

StoreStatus TimeSeriesStore::store_kline_batch(const std::string& symbol,
                                                uint8_t data_type,
                                                const std::vector<KlineRow>& rows) {
    if (rows.empty()) return StoreStatus::kInvalidArgument;
    cache_->append_batch(symbol, data_type, rows);
    return StoreStatus::kOk;
}

std::vector<KlineRow> TimeSeriesStore::query_kline(const std::string& symbol,
                                                    uint8_t data_type,
                                                    int64_t start_ts, int64_t end_ts) {
    return cache_->query(symbol, data_type, start_ts, end_ts);
}

// ── Coroutine API ──

CoTask<StoreStatus> TimeSeriesStore::co_store_kline(
    const std::string& symbol, uint8_t data_type, const KlineRow& row) {
    co_await cache_->co_append(symbol, data_type, row);
    co_return StoreStatus::kOk;
}

CoTask<StoreStatus> TimeSeriesStore::co_store_kline_batch(
    const std::string& symbol, uint8_t data_type,
    const std::vector<KlineRow>& rows) {
    if (rows.empty()) co_return StoreStatus::kInvalidArgument;
    co_await cache_->co_append_batch(symbol, data_type, rows);
    co_return StoreStatus::kOk;
}

CoTask<std::vector<KlineRow>> TimeSeriesStore::co_query_kline(
    const std::string& symbol, uint8_t data_type,
    int64_t start_ts, int64_t end_ts) {
    co_return co_await cache_->co_query(symbol, data_type, start_ts, end_ts);
}

StoreStatus TimeSeriesStore::flush() {
    disk_->flush();
    return StoreStatus::kOk;
}

StoreStatus TimeSeriesStore::close() {
    if (closed_) return StoreStatus::kOk;
    closed_ = true;
    disk_->flush();
    return StoreStatus::kOk;
}

}  // namespace quant::storage