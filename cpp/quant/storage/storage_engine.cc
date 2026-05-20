// storage_engine.cc — StorageEngine implementation
#include "cpp/quant/storage/storage_engine.h"

#include <algorithm>
#include <climits>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/network/co_io.h"

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
    uint8_t dt = static_cast<uint8_t>(kline_type);

    // Store each field as a single-element column block
    auto store_scalar = [&](DataField field, int64_t val) {
        int64_t arr[1] = {val};
        ColumnBlock block = ColumnBlock::compress(
            field, {arr, 1},
            ColumnBlock::Codec::kDelta, row.timestamp, row.timestamp);
        store_->put(symbol, dt, std::move(block));
    };

    store_scalar(DataField::kOpen, row.open_price);
    store_scalar(DataField::kHigh, row.high_price);
    store_scalar(DataField::kLow, row.low_price);
    store_scalar(DataField::kClose, row.close_price);
    store_scalar(DataField::kVolume, row.volume);
    store_scalar(DataField::kAmount, row.amount);
    store_scalar(DataField::kVwap, row.vwap);

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
    std::vector<int64_t> opens, highs, lows, closes;
    std::vector<int64_t> volumes, amounts, vwaps;

    timestamps.reserve(rows.size());
    opens.reserve(rows.size());
    highs.reserve(rows.size());
    lows.reserve(rows.size());
    closes.reserve(rows.size());
    volumes.reserve(rows.size());
    amounts.reserve(rows.size());
    vwaps.reserve(rows.size());

    int64_t min_ts = INT64_MAX, max_ts = INT64_MIN;
    for (const auto& row : rows) {
        timestamps.push_back(row.timestamp);
        opens.push_back(row.open_price);
        highs.push_back(row.high_price);
        lows.push_back(row.low_price);
        closes.push_back(row.close_price);
        volumes.push_back(row.volume);
        amounts.push_back(row.amount);
        vwaps.push_back(row.vwap);
        min_ts = std::min(min_ts, row.timestamp);
        max_ts = std::max(max_ts, row.timestamp);
    }

    // Compress and store each field as compressed blocks of kBlockSize
    auto store_field = [&](DataField field, const std::vector<int64_t>& values) {
        for (size_t i = 0; i < values.size(); i += ColumnBlock::kBlockSize) {
            size_t end = std::min(i + ColumnBlock::kBlockSize, values.size());
            auto block = ColumnBlock::compress(
                field, {values.data() + i, end - i},
                ColumnBlock::Codec::kDelta,
                timestamps[i], timestamps[end - 1]);
            store_->put(symbol, dt, std::move(block));
        }
    };

    store_field(DataField::kOpen, opens);
    store_field(DataField::kHigh, highs);
    store_field(DataField::kLow, lows);
    store_field(DataField::kClose, closes);
    store_field(DataField::kVolume, volumes);
    store_field(DataField::kAmount, amounts);
    store_field(DataField::kVwap, vwaps);

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

    // Decompress all blocks
    for (const auto& block : blocks) {
        size_t n = block.row_count();
        std::vector<double> values(n);
        size_t decoded = block.decompress(std::span<double>(values));
        if (decoded > 0) {
            values.resize(decoded);
            result.values.insert(result.values.end(),
                                 values.begin(), values.end());
        }
    }

    // Query timestamp blocks for the same time range
    auto ts_blocks = store_->query(symbol, dt, DataField::kOpen, range);
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
    return store_->flush();
}

StoreStatus StorageEngine::close() {
    if (closed_) return StoreStatus::kOk;
    closed_ = true;
    return store_->close();
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

    // 1. Query cache (sync — hot path, in-memory)
    auto cache_blocks = store_->cache().query(symbol, kline_type, field, range);
    for (const auto& block : cache_blocks) {
        size_t n = block.row_count();
        std::vector<double> values(n);
        size_t decoded = block.decompress(std::span<double>(values));
        if (decoded > 0) {
            values.resize(decoded);
            result.values.insert(result.values.end(),
                                 values.begin(), values.end());
        }
    }

    auto ts_cache = store_->cache().query(symbol, kline_type, DataField::kOpen, range);
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
            std::vector<double> values(n);
            size_t decoded = block.decompress(std::span<double>(values));
            if (decoded > 0) {
                values.resize(decoded);
                result.values.insert(result.values.end(),
                                     values.begin(), values.end());
            }
        }

        // Extract timestamps from kOpen blocks in the same segment
        for (const auto& block : blocks) {
            if (block.field() != DataField::kOpen) continue;
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
