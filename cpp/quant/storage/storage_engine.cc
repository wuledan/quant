// storage_engine.cc — StorageEngine implementation
#include "cpp/quant/storage/storage_engine.h"

#include <algorithm>
#include <climits>

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

}  // namespace quant::storage
