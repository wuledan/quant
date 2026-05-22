// storage_engine.cc — StorageEngine implementation
#include "cpp/quant/storage/storage_engine.h"

#include <algorithm>
#include <climits>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/network/co_io.h"
#include "cpp/quant/storage/write_buffer.h"

namespace quant::storage {

StorageEngine::StorageEngine(Options opts)
    : opts_(std::move(opts))
    , store_(std::make_unique<TimeSeriesStore>(
          opts_.cache_budget_mb, opts_.data_dir)) {}

StorageEngine::~StorageEngine() { close(); }

StoreStatus StorageEngine::store_kline(
    std::string_view symbol,
    quant::event::DataType kline_type,
    const quant::event::KlineRow& row) {
    // Route single-row writes through WriteBuffer if attached
    if (write_buffer_) {
        return write_buffer_->write(symbol, static_cast<uint8_t>(kline_type), row);
    }

    uint8_t dt = static_cast<uint8_t>(kline_type);

    // Timestamp: Delta codec with int64
    int64_t ts_arr[1] = {row.timestamp};
    ColumnBlock ts_block = ColumnBlock::compress(
        DataField::kTimestamp, {ts_arr, 1},
        ColumnBlock::Codec::kDelta, row.timestamp, row.timestamp);
    store_->put(symbol, dt, std::move(ts_block));

    // Price fields: Gorilla codec with double (int32 price / 10000.0)
    auto store_price = [&](DataField field, int32_t price_int) {
        double val = static_cast<double>(price_int) / 10000.0;
        double arr[1] = {val};
        ColumnBlock block = ColumnBlock::compress(
            field, {arr, 1},
            ColumnBlock::Codec::kGorilla, row.timestamp, row.timestamp);
        store_->put(symbol, dt, std::move(block));
    };

    // Integer fields: Delta codec with int64
    auto store_scalar = [&](DataField field, int64_t val) {
        int64_t arr[1] = {val};
        ColumnBlock block = ColumnBlock::compress(
            field, {arr, 1},
            ColumnBlock::Codec::kDelta, row.timestamp, row.timestamp);
        store_->put(symbol, dt, std::move(block));
    };

    store_price(DataField::kOpen, row.open_price);
    store_price(DataField::kHigh, row.high_price);
    store_price(DataField::kLow, row.low_price);
    store_price(DataField::kClose, row.close_price);
    store_price(DataField::kVwap, row.vwap);
    store_scalar(DataField::kVolume, row.volume);
    store_scalar(DataField::kAmount, row.amount);

    return StoreStatus::kOk;
}

StoreStatus StorageEngine::store_kline_batch(
    std::string_view symbol,
    quant::event::DataType kline_type,
    const std::vector<quant::event::KlineRow>& rows) {
    if (rows.empty()) return StoreStatus::kInvalidArgument;

    uint8_t dt = static_cast<uint8_t>(kline_type);

    // Collect per-field arrays
    std::vector<int64_t> timestamps;
    std::vector<double> opens, highs, lows, closes, vwaps;
    std::vector<int64_t> volumes, amounts;

    timestamps.reserve(rows.size());
    opens.reserve(rows.size());
    highs.reserve(rows.size());
    lows.reserve(rows.size());
    closes.reserve(rows.size());
    vwaps.reserve(rows.size());
    volumes.reserve(rows.size());
    amounts.reserve(rows.size());

    int64_t min_ts = INT64_MAX, max_ts = INT64_MIN;
    for (const auto& row : rows) {
        timestamps.push_back(row.timestamp);
        opens.push_back(static_cast<double>(row.open_price) / 10000.0);
        highs.push_back(static_cast<double>(row.high_price) / 10000.0);
        lows.push_back(static_cast<double>(row.low_price) / 10000.0);
        closes.push_back(static_cast<double>(row.close_price) / 10000.0);
        vwaps.push_back(static_cast<double>(row.vwap) / 10000.0);
        volumes.push_back(row.volume);
        amounts.push_back(row.amount);
        min_ts = std::min(min_ts, row.timestamp);
        max_ts = std::max(max_ts, row.timestamp);
    }

    // Price fields: Gorilla codec with double
    auto store_price_field = [&](DataField field, const std::vector<double>& values) {
        for (size_t i = 0; i < values.size(); i += ColumnBlock::kBlockSize) {
            size_t end = std::min(i + ColumnBlock::kBlockSize, values.size());
            auto block = ColumnBlock::compress(
                field, {values.data() + i, end - i},
                ColumnBlock::Codec::kGorilla,
                timestamps[i], timestamps[end - 1]);
            store_->put(symbol, dt, std::move(block));
        }
    };

    // Integer fields: Delta codec with int64
    auto store_int_field = [&](DataField field, const std::vector<int64_t>& values) {
        for (size_t i = 0; i < values.size(); i += ColumnBlock::kBlockSize) {
            size_t end = std::min(i + ColumnBlock::kBlockSize, values.size());
            auto block = ColumnBlock::compress(
                field, {values.data() + i, end - i},
                ColumnBlock::Codec::kDelta,
                timestamps[i], timestamps[end - 1]);
            store_->put(symbol, dt, std::move(block));
        }
    };

    store_price_field(DataField::kOpen, opens);
    store_price_field(DataField::kHigh, highs);
    store_price_field(DataField::kLow, lows);
    store_price_field(DataField::kClose, closes);
    store_price_field(DataField::kVwap, vwaps);
    store_int_field(DataField::kVolume, volumes);
    store_int_field(DataField::kAmount, amounts);

    // Timestamps: Delta codec with int64
    store_int_field(DataField::kTimestamp, timestamps);

    return StoreStatus::kOk;
}

StorageEngine::KlineQueryResult StorageEngine::query_kline(
    std::string_view symbol,
    quant::event::DataType kline_type,
    DataField field,
    TimeRange range) {
    KlineQueryResult result;

    uint8_t dt = static_cast<uint8_t>(kline_type);
    auto blocks = store_->query(symbol, dt, field, range);

    bool is_price_field = (field == DataField::kOpen ||
                           field == DataField::kHigh ||
                           field == DataField::kLow ||
                           field == DataField::kClose ||
                           field == DataField::kVwap);

    // Decompress all blocks
    for (const auto& block : blocks) {
        size_t n = block.row_count();
        if (is_price_field) {
            std::vector<double> values(n);
            size_t decoded = block.decompress(std::span<double>(values));
            if (decoded > 0) {
                values.resize(decoded);
                result.values.insert(result.values.end(),
                                     values.begin(), values.end());
            }
        } else {
            std::vector<int64_t> values(n);
            size_t decoded = block.decompress(std::span<int64_t>(values));
            if (decoded > 0) {
                values.resize(decoded);
                result.values.insert(result.values.end(),
                                     values.begin(), values.end());
            }
        }
    }

    // Query timestamp column
    auto ts_blocks = store_->query(symbol, dt, DataField::kTimestamp, range);
    for (const auto& block : ts_blocks) {
        size_t n = block.row_count();
        std::vector<int64_t> timestamps(n);
        size_t decoded = block.decompress(std::span<int64_t>(timestamps));
        if (decoded > 0) {
            timestamps.resize(decoded);
            result.timestamps.insert(result.timestamps.end(),
                                     timestamps.begin(), timestamps.end());
        }
    }

    return result;
}

StoreStatus StorageEngine::flush() {
    // Flush WriteBuffer first to ensure all pending writes are committed
    if (write_buffer_) {
        write_buffer_->flush();
    }
    return store_->flush();
}

StoreStatus StorageEngine::close() {
    if (closed_) return StoreStatus::kOk;
    closed_ = true;
    return store_->close();
}

void StorageEngine::set_write_buffer(std::unique_ptr<WriteBuffer> wb) {
    write_buffer_ = std::move(wb);
}

WriteBuffer* StorageEngine::write_buffer() noexcept {
    return write_buffer_.get();
}

// ── Coroutine API ──

void StorageEngine::set_io_uring(quant::network::CoIouring* ring) noexcept {
    store_->disk().set_io_uring(ring);
}

CoTask<StoreStatus> StorageEngine::co_store_kline(
    std::string_view symbol,
    quant::event::DataType kline_type,
    const quant::event::KlineRow& row) {
    // Cache put is an in-memory operation. Delegate to the sync path
    // (hot path stays sync). The CoTask wrapper allows callers to
    // co_await consistently in their coroutine chains.
    co_return store_kline(symbol, kline_type, row);
}

CoTask<StoreStatus> StorageEngine::co_store_kline_batch(
    std::string_view symbol,
    quant::event::DataType kline_type,
    const std::vector<quant::event::KlineRow>& rows) {
    if (rows.empty()) co_return StoreStatus::kInvalidArgument;
    co_return store_kline_batch(symbol, kline_type, rows);
}

CoTask<StorageEngine::KlineQueryResult> StorageEngine::co_query_kline(
    std::string_view symbol,
    quant::event::DataType kline_type,
    DataField field,
    TimeRange range) {
    KlineQueryResult result;
    uint8_t dt = static_cast<uint8_t>(kline_type);

    bool is_price_field = (field == DataField::kOpen ||
                           field == DataField::kHigh ||
                           field == DataField::kLow ||
                           field == DataField::kClose ||
                           field == DataField::kVwap);

    // 1. Query cache (sync — hot path, in-memory)
    auto cache_blocks = store_->cache().query(symbol, kline_type, field, range);
    for (const auto& block : cache_blocks) {
        size_t n = block.row_count();
        if (is_price_field) {
            std::vector<double> values(n);
            size_t decoded = block.decompress(std::span<double>(values));
            if (decoded > 0) {
                values.resize(decoded);
                result.values.insert(result.values.end(),
                                     values.begin(), values.end());
            }
        } else {
            std::vector<int64_t> values(n);
            size_t decoded = block.decompress(std::span<int64_t>(values));
            if (decoded > 0) {
                values.resize(decoded);
                result.values.insert(result.values.end(),
                                     values.begin(), values.end());
            }
        }
    }

    auto ts_cache = store_->cache().query(symbol, kline_type, DataField::kTimestamp, range);
    for (const auto& block : ts_cache) {
        size_t n = block.row_count();
        std::vector<int64_t> timestamps(n);
        size_t decoded = block.decompress(std::span<int64_t>(timestamps));
        if (decoded > 0) {
            timestamps.resize(decoded);
            result.timestamps.insert(result.timestamps.end(),
                                     timestamps.begin(), timestamps.end());
        }
    }

    // 2. Query disk segments via async io_uring reads
    auto& disk = store_->disk();
    auto segments = disk.list_segments(symbol, dt);
    for (const auto& seg : segments) {
        auto blocks = co_await disk.co_read_segment(seg);

        // Filter blocks matching the requested field and time range
        for (const auto& block : blocks) {
            if (block.field() != field) continue;
            if (block.max_timestamp() < range.begin_ts ||
                block.min_timestamp() > range.end_ts) continue;

            size_t n = block.row_count();
            if (is_price_field) {
                std::vector<double> values(n);
                size_t decoded = block.decompress(std::span<double>(values));
                if (decoded > 0) {
                    values.resize(decoded);
                    result.values.insert(result.values.end(),
                                         values.begin(), values.end());
                }
            } else {
                std::vector<int64_t> values(n);
                size_t decoded = block.decompress(std::span<int64_t>(values));
                if (decoded > 0) {
                    values.resize(decoded);
                    result.values.insert(result.values.end(),
                                         values.begin(), values.end());
                }
            }
        }

        // Extract timestamps from kTimestamp blocks in the same segment
        for (const auto& block : blocks) {
            if (block.field() != DataField::kTimestamp) continue;
            if (block.max_timestamp() < range.begin_ts ||
                block.min_timestamp() > range.end_ts) continue;

            size_t n = block.row_count();
            std::vector<int64_t> timestamps(n);
            size_t decoded = block.decompress(std::span<int64_t>(timestamps));
            if (decoded > 0) {
                timestamps.resize(decoded);
                result.timestamps.insert(result.timestamps.end(),
                                         timestamps.begin(), timestamps.end());
            }
        }
    }

    co_return result;
}

CoTask<StoreStatus> StorageEngine::co_flush() {
    if (closed_) co_return StoreStatus::kStorageFull;
    store_->disk().flush();
    co_return StoreStatus::kOk;
}

}  // namespace quant::storage
