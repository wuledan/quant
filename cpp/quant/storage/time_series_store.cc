// time_series_store.cc — Per-symbol kline storage (cache + disk)
#include "cpp/quant/storage/time_series_store.h"

#include <climits>
#include <map>

namespace quant::storage {

TimeSeriesStore::TimeSeriesStore(Options opts)
    : opts_(std::move(opts))
    , cache_(std::make_unique<TimeSeriesCache>(opts_.cache_opts))
    , disk_(std::make_unique<DiskPersistence>(opts_.data_dir)) {}

TimeSeriesStore::~TimeSeriesStore() { close(); }

// ── Helpers ──

// Build a composite key for the pending_disk_ map
static std::string pending_key(const std::string& symbol, uint8_t data_type) {
    std::string k;
    k.reserve(symbol.size() + 2);
    k += symbol;
    k += '\0';
    k += static_cast<char>(data_type);
    return k;
}

// Convert rows to 8 column blocks: ts, open, high, low, close, vwap, volume, amount
std::vector<ColumnBlock> TimeSeriesStore::rows_to_column_blocks(
    const std::vector<KlineRow>& rows) {
    const size_t n = rows.size();
    std::vector<int64_t> timestamps(n);
    std::vector<int32_t> open(n), high(n), low(n), close_src(n), vwap(n);
    std::vector<int64_t> volume(n), amount(n);

    for (size_t i = 0; i < n; ++i) {
        timestamps[i] = rows[i].timestamp;
        open[i]   = rows[i].open_price;
        high[i]   = rows[i].high_price;
        low[i]    = rows[i].low_price;
        close_src[i] = rows[i].close_price;
        vwap[i]   = rows[i].vwap;
        volume[i] = rows[i].volume;
        amount[i] = rows[i].amount;
    }

    int64_t min_ts = timestamps.front();
    int64_t max_ts = timestamps.back();

    std::vector<ColumnBlock> blocks;
    blocks.reserve(8);
    blocks.push_back(ColumnBlock::compress(DataField::kTimestamp, timestamps,
                                           ColumnBlock::Codec::kDelta, min_ts, max_ts));
    blocks.push_back(ColumnBlock::compress(DataField::kOpen, open,
                                           ColumnBlock::Codec::kDelta, min_ts, max_ts));
    blocks.push_back(ColumnBlock::compress(DataField::kHigh, high,
                                           ColumnBlock::Codec::kDelta, min_ts, max_ts));
    blocks.push_back(ColumnBlock::compress(DataField::kLow, low,
                                           ColumnBlock::Codec::kDelta, min_ts, max_ts));
    blocks.push_back(ColumnBlock::compress(DataField::kClose, close_src,
                                           ColumnBlock::Codec::kDelta, min_ts, max_ts));
    blocks.push_back(ColumnBlock::compress(DataField::kVwap, vwap,
                                           ColumnBlock::Codec::kDelta, min_ts, max_ts));
    blocks.push_back(ColumnBlock::compress(DataField::kVolume, volume,
                                           ColumnBlock::Codec::kDelta, min_ts, max_ts));
    blocks.push_back(ColumnBlock::compress(DataField::kAmount, amount,
                                           ColumnBlock::Codec::kDelta, min_ts, max_ts));
    return blocks;
}

std::vector<KlineRow> TimeSeriesStore::blocks_to_rows(
    const std::vector<ColumnBlock>& blocks) {
    if (blocks.empty()) return {};

    // Find timestamp block to determine row count
    size_t n = 0;
    for (const auto& b : blocks) {
        if (b.field() == DataField::kTimestamp) {
            n = b.row_count();
            break;
        }
    }
    if (n == 0) return {};

    // Decompress each field into its own vector
    std::vector<int64_t> timestamps(n);
    std::vector<int32_t> open(n), high(n), low(n), close_src(n), vwap(n);
    std::vector<int64_t> volume(n), amount(n);

    for (const auto& b : blocks) {
        switch (b.field()) {
            case DataField::kTimestamp:
                b.decompress(std::span<int64_t>(timestamps));
                break;
            case DataField::kOpen:
                b.decompress(std::span<int32_t>(open));
                break;
            case DataField::kHigh:
                b.decompress(std::span<int32_t>(high));
                break;
            case DataField::kLow:
                b.decompress(std::span<int32_t>(low));
                break;
            case DataField::kClose:
                b.decompress(std::span<int32_t>(close_src));
                break;
            case DataField::kVwap:
                b.decompress(std::span<int32_t>(vwap));
                break;
            case DataField::kVolume:
                b.decompress(std::span<int64_t>(volume));
                break;
            case DataField::kAmount:
                b.decompress(std::span<int64_t>(amount));
                break;
            default:
                break;
        }
    }

    // Assemble rows
    std::vector<KlineRow> rows;
    rows.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        KlineRow row;
        row.timestamp = timestamps[i];
        row.open_price = open[i];
        row.high_price = high[i];
        row.low_price = low[i];
        row.close_price = close_src[i];
        row.vwap = vwap[i];
        row.volume = volume[i];
        row.amount = amount[i];
        rows.push_back(std::move(row));
    }
    return rows;
}

CoTask<void> TimeSeriesStore::flush_to_disk(const std::string& symbol,
                                             uint8_t data_type,
                                             PendingDisk& batch) {
    auto blocks = rows_to_column_blocks(batch.rows);
    co_await disk_->co_write_segment(symbol, data_type, blocks,
                                     batch.min_ts, batch.max_ts);
    batch.rows.clear();
    batch.rows.shrink_to_fit();
    batch.min_ts = INT64_MAX;
    batch.max_ts = INT64_MIN;
}

size_t TimeSeriesStore::pending_disk_rows() const noexcept {
    size_t total = 0;
    for (const auto& [key, batch] : pending_disk_) {
        total += batch.rows.size();
    }
    return total;
}

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

    // Accumulate for disk flush (same as coroutine path)
    std::string key = pending_key(symbol, data_type);
    auto& batch = pending_disk_[key];
    if (batch.rows.empty()) {
        batch.min_ts = rows.front().timestamp;
        batch.max_ts = rows.back().timestamp;
    } else {
        batch.min_ts = std::min(batch.min_ts, rows.front().timestamp);
        batch.max_ts = std::max(batch.max_ts, rows.back().timestamp);
    }
    batch.rows.insert(batch.rows.end(), rows.begin(), rows.end());

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

    // Accumulate for disk flush (periodic or threshold-based)
    std::string key = pending_key(symbol, data_type);
    auto& batch = pending_disk_[key];
    if (batch.rows.empty()) {
        batch.min_ts = row.timestamp;
        batch.max_ts = row.timestamp;
    } else {
        batch.min_ts = std::min(batch.min_ts, row.timestamp);
        batch.max_ts = std::max(batch.max_ts, row.timestamp);
    }
    batch.rows.push_back(row);

    co_return StoreStatus::kOk;
}

CoTask<StoreStatus> TimeSeriesStore::co_store_kline_batch(
    const std::string& symbol, uint8_t data_type,
    const std::vector<KlineRow>& rows) {
    if (rows.empty()) co_return StoreStatus::kInvalidArgument;

    // 1. Write to cache first (hot data always in memory)
    co_await cache_->co_append_batch(symbol, data_type, rows);

    // 2. Accumulate for disk flush
    std::string key = pending_key(symbol, data_type);
    auto& batch = pending_disk_[key];

    if (batch.rows.empty()) {
        batch.min_ts = rows.front().timestamp;
        batch.max_ts = rows.back().timestamp;
    } else {
        batch.min_ts = std::min(batch.min_ts, rows.front().timestamp);
        batch.max_ts = std::max(batch.max_ts, rows.back().timestamp);
    }
    batch.rows.insert(batch.rows.end(), rows.begin(), rows.end());

    // 3. Flush to disk when threshold reached
    if (batch.rows.size() >= kFlushThreshold) {
        co_await flush_to_disk(symbol, data_type, batch);
    }

    co_return StoreStatus::kOk;
}

CoTask<std::vector<KlineRow>> TimeSeriesStore::co_query_kline(
    const std::string& symbol, uint8_t data_type,
    int64_t start_ts, int64_t end_ts) {
    // 1. Query cache first
    auto cached = co_await cache_->co_query(symbol, data_type, start_ts, end_ts);
    if (!cached.empty()) co_return cached;

    // 2. Cache miss: query SegmentIndex for matching disk segments
    auto metas = co_await disk_->index().co_query(symbol, data_type,
        DataField::kClose, start_ts, end_ts);

    // 2a. Remote read-through: when disk has no data and a remote
    //     storage is configured, download from S3, persist locally,
    //     backfill cache, and return the range slice.
    if (metas.empty() && remote_storage_) {
        int start_year = RemoteStorage::year_from_timestamp(start_ts);
        int end_year = RemoteStorage::year_from_timestamp(end_ts);

        std::map<int64_t, KlineRow> merged;
        for (int year = start_year; year <= end_year; ++year) {
            if (!remote_storage_->exists(symbol, data_type, year)) continue;

            auto rows = remote_storage_->download_kline(symbol, data_type, year);
            if (rows.empty()) continue;

            // Persist to local disk so subsequent queries hit disk
            auto blocks = rows_to_column_blocks(rows);
            int64_t min_ts = rows.front().timestamp;
            int64_t max_ts = rows.back().timestamp;
            co_await disk_->co_write_segment(
                symbol, data_type, blocks, min_ts, max_ts);

            // Backfill cache with remote-source priority
            co_await cache_->co_append_batch(
                symbol, data_type, rows, DataSource::kRemoteLoad);

            for (auto& row : rows) {
                merged[row.timestamp] = std::move(row);
            }
        }

        if (!merged.empty()) {
            std::vector<KlineRow> result;
            auto lo = merged.lower_bound(start_ts);
            auto hi = merged.upper_bound(end_ts);
            for (auto it = lo; it != hi; ++it) {
                result.push_back(it->second);
            }
            co_return result;
        }
        co_return {};
    }

    if (metas.empty()) co_return {};

    // 3. Collect unique segment filenames (each segment has multiple field entries)
    std::vector<std::string> filenames;
    {
        std::string_view last;
        for (const auto& m : metas) {
            if (m.file_path != last) {
                filenames.push_back(m.file_path);
                last = m.file_path;
            }
        }
    }

    // 4. Read segments from disk, decompress, merge by timestamp (dedup across segments)
    std::map<int64_t, KlineRow> merged;
    for (const auto& fname : filenames) {
        auto blocks = co_await disk_->co_read_segment(fname);
        if (blocks.empty()) continue;

        auto rows = blocks_to_rows(blocks);
        for (auto& row : rows) {
            merged[row.timestamp] = std::move(row);
        }
    }

    if (merged.empty()) co_return {};

    // 5. Extract time-range slice
    std::vector<KlineRow> result;
    auto lo = merged.lower_bound(start_ts);
    auto hi = merged.upper_bound(end_ts);
    for (auto it = lo; it != hi; ++it) {
        result.push_back(it->second);
    }

    // 6. Backfill cache with DataSource::kBatchLoad
    if (!result.empty()) {
        co_await cache_->co_append_batch(symbol, data_type, result,
                                          DataSource::kBatchLoad);
    }

    co_return result;
}

StoreStatus TimeSeriesStore::flush() {
    disk_->flush();
    return StoreStatus::kOk;
}

CoTask<void> TimeSeriesStore::co_flush() {
    for (auto& [key, batch] : pending_disk_) {
        if (batch.rows.empty()) continue;
        auto sep = key.find('\0');
        if (sep == std::string::npos) continue;
        std::string symbol = key.substr(0, sep);
        uint8_t data_type = static_cast<uint8_t>(key[sep + 1]);
        co_await flush_to_disk(symbol, data_type, batch);
    }
    pending_disk_.clear();
    disk_->flush();
}

StoreStatus TimeSeriesStore::close() {
    if (closed_) return StoreStatus::kOk;
    closed_ = true;

    // Flush all remaining pending data to disk via the sync path
    // (close() is not a coroutine, so we use blockinWait for the coroutine path)
    for (auto& [key, batch] : pending_disk_) {
        if (batch.rows.empty()) continue;
        // Parse symbol+data_type from the composite key
        auto sep = key.find('\0');
        if (sep == std::string::npos) continue;
        std::string symbol = key.substr(0, sep);
        uint8_t data_type = static_cast<uint8_t>(key[sep + 1]);
        infra::blockingWait(flush_to_disk(symbol, data_type, batch));
    }
    pending_disk_.clear();

    disk_->flush();
    return StoreStatus::kOk;
}

}  // namespace quant::storage