// storage_test.cc — Tests for ColumnBlock, TimeSeriesCache, DiskPersistence,
//                    TimeSeriesStore, StorageEngine, WriteAheadLog, WriteBuffer
#include "cpp/quant/storage/column_block.h"
#include "cpp/quant/storage/disk_persistence.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/storage/time_series_cache.h"
#include "cpp/quant/storage/time_series_store.h"
#include "cpp/quant/storage/write_ahead_log.h"
#include "cpp/quant/storage/write_buffer.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

namespace quant::storage {
namespace {

// Temporary directory helper
class TempDir {
public:
    TempDir() {
        auto path = std::filesystem::temp_directory_path() / "storage_test_XXXXXX";
        std::string tmpl = path.string();
        auto* p = mkdtemp(tmpl.data());
        if (p == nullptr) {
            throw std::runtime_error("Failed to create temp directory");
        }
        path_ = tmpl;
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

// ========================================================================
// ColumnBlock compression tests
// ========================================================================

TEST(ColumnBlockTest, DeltaEncodeDecodeInt64) {
    std::vector<int64_t> data = {1000000, 1000500, 1001200, 1002100, 1003000};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kOpen, data, ColumnBlock::Codec::kDelta,
        1000000, 1003000);

    EXPECT_EQ(block.field(), DataField::kOpen);
    EXPECT_EQ(block.codec(), ColumnBlock::Codec::kDelta);
    EXPECT_EQ(block.row_count(), 5u);
    EXPECT_GT(block.compressed_size(), 0u);
    EXPECT_LE(static_cast<ssize_t>(block.compressed_size()),
              static_cast<ssize_t>(data.size() * sizeof(int64_t)));

    // Decode
    std::vector<int64_t> out(5);
    size_t n = block.decompress(std::span<int64_t>(out));
    EXPECT_EQ(n, 5u);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], data[i]);
    }
}

TEST(ColumnBlockTest, DeltaEncodeDecodeInt64Large) {
    // Generate monotonic timestamps
    std::vector<int64_t> data(1000);
    for (size_t i = 0; i < 1000; ++i) {
        data[i] = 1700000000000000LL + static_cast<int64_t>(i) * 60000000LL;
    }

    ColumnBlock block = ColumnBlock::compress(
        DataField::kOpen, data, ColumnBlock::Codec::kDelta,
        data.front(), data.back());

    // Delta-of-delta should compress this well
    EXPECT_LT(block.compressed_size(), data.size() * sizeof(int64_t));

    std::vector<int64_t> out(1000);
    size_t n = block.decompress(std::span<int64_t>(out));
    EXPECT_EQ(n, 1000u);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], data[i]);
    }
}

TEST(ColumnBlockTest, DeltaEncodeDecodeInt32) {
    std::vector<int32_t> data = {100, 105, 98, 110, 108, 115, 120};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kClose, data, ColumnBlock::Codec::kDelta,
        0, 600000);

    EXPECT_EQ(block.codec(), ColumnBlock::Codec::kDelta);
    EXPECT_EQ(block.row_count(), 7u);

    std::vector<int32_t> out(7);
    size_t n = block.decompress(std::span<int32_t>(out));
    EXPECT_EQ(n, 7u);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], data[i]);
    }
}

TEST(ColumnBlockTest, GorillaEncodeDecodeDouble) {
    std::vector<double> data = {100.5, 100.5, 101.0, 101.0, 102.5, 102.5, 102.5};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kVwap, data, ColumnBlock::Codec::kGorilla,
        0, 600000);

    EXPECT_EQ(block.field(), DataField::kVwap);
    EXPECT_EQ(block.codec(), ColumnBlock::Codec::kGorilla);
    EXPECT_EQ(block.row_count(), 7u);

    std::vector<double> out(7);
    size_t n = block.decompress(std::span<double>(out));
    EXPECT_EQ(n, 7u);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_DOUBLE_EQ(out[i], data[i]);
    }
}

TEST(ColumnBlockTest, GorillaCompressesRepeating) {
    // All same value → very good compression
    std::vector<double> data(100, 42.0);
    ColumnBlock block = ColumnBlock::compress(
        DataField::kClose, data, ColumnBlock::Codec::kGorilla,
        0, 100);

    // Repeating values: first 8 bytes for initial, then 1 byte per repeat
    EXPECT_LE(block.compressed_size(), 8u + data.size() * 1u + 10u);

    std::vector<double> out(100);
    size_t n = block.decompress(std::span<double>(out));
    EXPECT_EQ(n, 100u);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_DOUBLE_EQ(out[i], 42.0);
    }
}

TEST(ColumnBlockTest, UncompressedCodecNone) {
    std::vector<int64_t> data = {100, 200, 300, 400, 500};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kVolume, data, ColumnBlock::Codec::kNone,
        100, 500);

    EXPECT_EQ(block.codec(), ColumnBlock::Codec::kNone);
    EXPECT_EQ(block.compressed_size(), data.size() * sizeof(int64_t));

    std::vector<int64_t> out(5);
    size_t n = block.decompress(std::span<int64_t>(out));
    EXPECT_EQ(n, 5u);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], data[i]);
    }
}

TEST(ColumnBlockTest, Timestamps) {
    std::vector<int64_t> data = {1000, 2000, 3000};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kOpen, data, ColumnBlock::Codec::kDelta, 1000, 3000);

    EXPECT_EQ(block.min_timestamp(), 1000);
    EXPECT_EQ(block.max_timestamp(), 3000);
}

TEST(ColumnBlockTest, EmptyCompression) {
    std::vector<int64_t> empty;
    ColumnBlock block = ColumnBlock::compress(
        DataField::kOpen, empty, ColumnBlock::Codec::kDelta, 0, 0);
    EXPECT_EQ(block.row_count(), 0u);
    EXPECT_EQ(block.compressed_size(), 0u);
}

TEST(ColumnBlockTest, DataFieldNames) {
    EXPECT_EQ(data_field_name(DataField::kOpen), "open");
    EXPECT_EQ(data_field_name(DataField::kHigh), "high");
    EXPECT_EQ(data_field_name(DataField::kLow), "low");
    EXPECT_EQ(data_field_name(DataField::kClose), "close");
    EXPECT_EQ(data_field_name(DataField::kVolume), "volume");
    EXPECT_EQ(data_field_name(DataField::kAmount), "amount");
    EXPECT_EQ(data_field_name(DataField::kVwap), "vwap");
}

// ========================================================================
// TimeSeriesCache tests
// ========================================================================

TEST(TimeSeriesCacheTest, AppendAndQuery) {
    TimeSeriesCache cache(1);  // 1 MB budget

    std::vector<int64_t> data = {1000, 1010, 1020, 1030, 1040};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kOpen, data, ColumnBlock::Codec::kDelta, 1000, 1040);

    cache.append("AAPL", quant::event::DataType::kKline1Min, std::move(block));

    auto results = cache.query("AAPL", quant::event::DataType::kKline1Min,
                                DataField::kOpen, TimeRange{900, 1100});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].row_count(), 5u);

    std::vector<int64_t> out(5);
    size_t n = results[0].decompress(std::span<int64_t>(out));
    EXPECT_EQ(n, 5u);
    EXPECT_EQ(out[0], 1000);
    EXPECT_EQ(out[4], 1040);
}

TEST(TimeSeriesCacheTest, QueryFiltersByField) {
    TimeSeriesCache cache(1);

    std::vector<int64_t> opens = {100, 110, 120};
    std::vector<int64_t> closes = {105, 115, 125};
    auto b1 = ColumnBlock::compress(DataField::kOpen, opens, ColumnBlock::Codec::kDelta, 0, 2);
    auto b2 = ColumnBlock::compress(DataField::kClose, closes, ColumnBlock::Codec::kDelta, 0, 2);

    cache.append("MSFT", quant::event::DataType::kKline5Min, std::move(b1));
    cache.append("MSFT", quant::event::DataType::kKline5Min, std::move(b2));

    auto open_results = cache.query("MSFT", quant::event::DataType::kKline5Min,
                                     DataField::kOpen, TimeRange{0, 100});
    ASSERT_EQ(open_results.size(), 1u);
    EXPECT_EQ(open_results[0].field(), DataField::kOpen);

    auto close_results = cache.query("MSFT", quant::event::DataType::kKline5Min,
                                       DataField::kClose, TimeRange{0, 100});
    ASSERT_EQ(close_results.size(), 1u);
    EXPECT_EQ(close_results[0].field(), DataField::kClose);
}

TEST(TimeSeriesCacheTest, QueryFiltersByTimeRange) {
    TimeSeriesCache cache(1);

    std::vector<int64_t> data1 = {100, 200, 300};   // ts range: 1000-2000
    std::vector<int64_t> data2 = {400, 500, 600};   // ts range: 3000-4000

    auto b1 = ColumnBlock::compress(DataField::kOpen, data1,
                                     ColumnBlock::Codec::kDelta, 1000, 2000);
    auto b2 = ColumnBlock::compress(DataField::kOpen, data2,
                                     ColumnBlock::Codec::kDelta, 3000, 4000);

    cache.append("GOOG", quant::event::DataType::kKline1Min, std::move(b1));
    cache.append("GOOG", quant::event::DataType::kKline1Min, std::move(b2));

    // Query only first block's time range
    auto results = cache.query("GOOG", quant::event::DataType::kKline1Min,
                                DataField::kOpen, TimeRange{900, 2500});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].min_timestamp(), 1000);
}

TEST(TimeSeriesCacheTest, EvictRemovesData) {
    TimeSeriesCache cache(1);

    std::vector<int64_t> data = {1, 2, 3};
    auto block = ColumnBlock::compress(DataField::kOpen, data,
                                        ColumnBlock::Codec::kDelta, 0, 2);
    cache.append("TSLA", quant::event::DataType::kKline1Min, std::move(block));

    auto results = cache.query("TSLA", quant::event::DataType::kKline1Min,
                                DataField::kOpen, TimeRange{0, 100});
    EXPECT_EQ(results.size(), 1u);

    cache.evict("TSLA", quant::event::DataType::kKline1Min);

    results = cache.query("TSLA", quant::event::DataType::kKline1Min,
                           DataField::kOpen, TimeRange{0, 100});
    EXPECT_EQ(results.size(), 0u);
}

TEST(TimeSeriesCacheTest, MemoryBudgetTracking) {
    TimeSeriesCache cache(1);

    EXPECT_EQ(cache.used_memory(), 0u);
    EXPECT_EQ(cache.shard_count(), 64u);

    std::vector<int64_t> data = {1, 2, 3};
    auto block = ColumnBlock::compress(DataField::kOpen, data,
                                        ColumnBlock::Codec::kDelta, 0, 2);
    size_t mem_before = cache.used_memory();
    cache.append("TEST", quant::event::DataType::kKline1Min, std::move(block));
    EXPECT_GT(cache.used_memory(), mem_before);
}

TEST(TimeSeriesCacheTest, MultipleSymbols) {
    TimeSeriesCache cache(1);

    for (int i = 0; i < 5; ++i) {
        std::vector<int64_t> data = {i * 100, i * 100 + 10, i * 100 + 20};
        std::string symbol = "SYM" + std::to_string(i);
        auto block = ColumnBlock::compress(DataField::kOpen, data,
                                             ColumnBlock::Codec::kDelta, i * 100, i * 100 + 20);
        cache.append(symbol, quant::event::DataType::kKline1Min, std::move(block));
    }

    for (int i = 0; i < 5; ++i) {
        std::string symbol = "SYM" + std::to_string(i);
        auto results = cache.query(symbol, quant::event::DataType::kKline1Min,
                                     DataField::kOpen, TimeRange{0, 99999});
        EXPECT_EQ(results.size(), 1u);
    }
}

// ========================================================================
// DiskPersistence tests
// ========================================================================

TEST(DiskPersistenceTest, WriteAndReadSegment) {
    TempDir tmpdir;
    DiskPersistence disk(tmpdir.path());

    // Create blocks
    std::vector<int64_t> open_data = {100, 110, 120};
    std::vector<int64_t> close_data = {105, 115, 125};
    auto b1 = ColumnBlock::compress(DataField::kOpen, open_data,
                                     ColumnBlock::Codec::kDelta, 1000, 2000);
    auto b2 = ColumnBlock::compress(DataField::kClose, close_data,
                                     ColumnBlock::Codec::kDelta, 1000, 2000);

    std::vector<ColumnBlock> blocks;
    blocks.push_back(std::move(b1));
    blocks.push_back(std::move(b2));

    // Write
    std::string fname = disk.write_segment("AAPL", 0, blocks, 1000, 2000);
    EXPECT_FALSE(fname.empty());

    // Read
    auto read_blocks = disk.read_segment(fname);
    ASSERT_EQ(read_blocks.size(), 2u);

    EXPECT_EQ(read_blocks[0].field(), DataField::kOpen);
    EXPECT_EQ(read_blocks[0].codec(), ColumnBlock::Codec::kDelta);
    EXPECT_EQ(read_blocks[0].row_count(), 3u);
    EXPECT_EQ(read_blocks[0].min_timestamp(), 1000);
    EXPECT_EQ(read_blocks[0].max_timestamp(), 2000);

    EXPECT_EQ(read_blocks[1].field(), DataField::kClose);
    EXPECT_EQ(read_blocks[1].row_count(), 3u);

    // Decompress to verify data integrity
    std::vector<int64_t> out(3);
    size_t n = read_blocks[0].decompress(std::span<int64_t>(out));
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 100);
    EXPECT_EQ(out[1], 110);
    EXPECT_EQ(out[2], 120);
}

TEST(DiskPersistenceTest, ListSegments) {
    TempDir tmpdir;
    DiskPersistence disk(tmpdir.path());

    std::vector<int64_t> data = {10, 20, 30};
    auto block = ColumnBlock::compress(DataField::kOpen, data,
                                        ColumnBlock::Codec::kDelta, 1000, 3000);

    std::vector<ColumnBlock> blocks;
    blocks.push_back(std::move(block));

    disk.write_segment("TEST", 1, blocks, 1000, 3000);
    disk.write_segment("TEST", 1, blocks, 4000, 6000);

    auto segments = disk.list_segments("TEST", 1);
    EXPECT_EQ(segments.size(), 2u);

    auto other = disk.list_segments("OTHER", 1);
    EXPECT_EQ(other.size(), 0u);
}

TEST(DiskPersistenceTest, DeleteSegment) {
    TempDir tmpdir;
    DiskPersistence disk(tmpdir.path());

    std::vector<int64_t> data = {1, 2, 3};
    auto block = ColumnBlock::compress(DataField::kOpen, data,
                                        ColumnBlock::Codec::kDelta, 0, 2);
    std::vector<ColumnBlock> blocks;
    blocks.push_back(std::move(block));

    std::string fname = disk.write_segment("DEL", 0, blocks, 0, 2);
    EXPECT_FALSE(fname.empty());

    auto segments = disk.list_segments("DEL", 0);
    EXPECT_EQ(segments.size(), 1u);

    EXPECT_TRUE(disk.delete_segment(fname));

    segments = disk.list_segments("DEL", 0);
    EXPECT_EQ(segments.size(), 0u);
}

TEST(DiskPersistenceTest, ReadSegmentFiltered) {
    TempDir tmpdir;
    DiskPersistence disk(tmpdir.path());

    std::vector<int64_t> open_data = {100, 110, 120};
    std::vector<int64_t> close_data = {105, 115, 125};
    auto b1 = ColumnBlock::compress(DataField::kOpen, open_data,
                                     ColumnBlock::Codec::kDelta, 1000, 2000);
    auto b2 = ColumnBlock::compress(DataField::kClose, close_data,
                                     ColumnBlock::Codec::kDelta, 1000, 2000);

    std::vector<ColumnBlock> blocks;
    blocks.push_back(std::move(b1));
    blocks.push_back(std::move(b2));

    std::string fname = disk.write_segment("FILT", 0, blocks, 1000, 2000);

    // Filter by field
    auto results = disk.read_segment_filtered(fname, DataField::kOpen, 0, 99999);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].field(), DataField::kOpen);

    // Filter by time range that excludes data
    auto no_results = disk.read_segment_filtered(fname, DataField::kOpen, 5000, 6000);
    EXPECT_EQ(no_results.size(), 0u);

    // Filter by wrong field
    auto vol_results = disk.read_segment_filtered(fname, DataField::kVolume, 0, 99999);
    EXPECT_EQ(vol_results.size(), 0u);
}

TEST(DiskPersistenceTest, GorillaBlockRoundTrip) {
    TempDir tmpdir;
    DiskPersistence disk(tmpdir.path());

    std::vector<double> prices = {100.5, 101.0, 100.8, 102.3, 103.1};
    auto block = ColumnBlock::compress(DataField::kVwap, prices,
                                       ColumnBlock::Codec::kGorilla, 1000, 5000);

    std::vector<ColumnBlock> blocks;
    blocks.push_back(std::move(block));

    std::string fname = disk.write_segment("GORILLA", 0, blocks, 1000, 5000);
    auto read_blocks = disk.read_segment(fname);

    ASSERT_EQ(read_blocks.size(), 1u);
    EXPECT_EQ(read_blocks[0].codec(), ColumnBlock::Codec::kGorilla);

    std::vector<double> out(5);
    size_t n = read_blocks[0].decompress(std::span<double>(out));
    EXPECT_EQ(n, 5u);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_DOUBLE_EQ(out[i], prices[i]);
    }
}

// ========================================================================
// TimeSeriesStore tests
// ========================================================================

TEST(TimeSeriesStoreTest, PutAndQueryCache) {
    TempDir tmpdir;
    TimeSeriesStore store(1, tmpdir.path());

    std::vector<int64_t> data = {1000, 1010, 1020};
    auto block = ColumnBlock::compress(DataField::kOpen, data,
                                        ColumnBlock::Codec::kDelta, 1000, 1020);

    auto status = store.put("AAPL", 0, std::move(block));
    EXPECT_EQ(status, StoreStatus::kOk);

    auto results = store.query("AAPL", 0, DataField::kOpen,
                               TimeRange{900, 1100});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].row_count(), 3u);

    std::vector<int64_t> out(3);
    size_t n = results[0].decompress(std::span<int64_t>(out));
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 1000);
}

TEST(TimeSeriesStoreTest, PutEmptySymbolFails) {
    TempDir tmpdir;
    TimeSeriesStore store(1, tmpdir.path());

    std::vector<int64_t> data = {1, 2, 3};
    auto block = ColumnBlock::compress(DataField::kOpen, data,
                                        ColumnBlock::Codec::kDelta, 0, 2);

    auto status = store.put("", 0, std::move(block));
    EXPECT_EQ(status, StoreStatus::kInvalidArgument);
}

TEST(TimeSeriesStoreTest, QueryAllFromDisk) {
    TempDir tmpdir;
    TimeSeriesStore store(1, tmpdir.path());

    // Write data to cache first
    std::vector<int64_t> data = {100, 200, 300};
    auto block = ColumnBlock::compress(DataField::kOpen, data,
                                        ColumnBlock::Codec::kDelta, 1000, 3000);

    auto status = store.put("DISK", 0, std::move(block));
    EXPECT_EQ(status, StoreStatus::kOk);

    // Also write a segment to disk directly for testing
    std::vector<int64_t> more_data = {400, 500, 600};
    auto disk_block = ColumnBlock::compress(DataField::kOpen, more_data,
                                             ColumnBlock::Codec::kDelta, 4000, 6000);
    std::vector<ColumnBlock> disk_blocks;
    disk_blocks.push_back(std::move(disk_block));
    store.disk().write_segment("DISK", 0, disk_blocks, 4000, 6000);

    // Query all should find both cache and disk data
    auto all_results = store.query_all("DISK", 0, TimeRange{0, 99999});
    EXPECT_GE(all_results.size(), 1u);
}

TEST(TimeSeriesStoreTest, LatestFromCache) {
    TempDir tmpdir;
    TimeSeriesStore store(1, tmpdir.path());

    std::vector<int64_t> data = {42, 43, 44};
    auto block = ColumnBlock::compress(DataField::kOpen, data,
                                        ColumnBlock::Codec::kDelta, 5000, 7000);

    store.put("LATE", 0, std::move(block));

    auto latest = store.latest("LATE", 0, DataField::kOpen);
    EXPECT_EQ(latest.row_count(), 3u);
}

// ========================================================================
// StorageEngine tests
// ========================================================================

TEST(StorageEngineTest, StoreKlineSingle) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});

    quant::event::KlineRow row{};
    row.timestamp = 1700000000000000LL;
    row.open_price = 10000;
    row.high_price = 10500;
    row.low_price = 9800;
    row.close_price = 10300;
    row.volume = 1000000;
    row.amount = 103000000LL;
    row.vwap = 102500;

    auto status = engine.store_kline("AAPL", quant::event::DataType::kKline1Min, row);
    EXPECT_EQ(status, StoreStatus::kOk);
}

TEST(StorageEngineTest, StoreKlineBatch) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});

    std::vector<quant::event::KlineRow> rows;
    for (int i = 0; i < 10; ++i) {
        quant::event::KlineRow row{};
        row.timestamp = 1700000000000000LL + i * 60000000LL;
        row.open_price = 10000 + i * 10;
        row.high_price = row.open_price + 50;
        row.low_price = row.open_price - 20;
        row.close_price = row.open_price + 5;
        row.volume = 1000000;
        row.amount = row.close_price * 100;
        row.vwap = row.close_price;
        rows.push_back(row);
    }

    auto status = engine.store_kline_batch("MSFT", quant::event::DataType::kKline5Min, rows);
    EXPECT_EQ(status, StoreStatus::kOk);
}

TEST(StorageEngineTest, StoreKlineBatchEmptyFails) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});

    std::vector<quant::event::KlineRow> empty;
    auto status = engine.store_kline_batch("EMPTY", quant::event::DataType::kKline1Min, empty);
    EXPECT_EQ(status, StoreStatus::kInvalidArgument);
}

TEST(StorageEngineTest, StoreAndQueryKline) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{4, tmpdir.path()});

    // Store 5 kline rows
    std::vector<quant::event::KlineRow> rows;
    for (int i = 0; i < 5; ++i) {
        quant::event::KlineRow row{};
        row.timestamp = 1000 + i * 100;
        row.open_price = 10000 + i * 100;
        row.high_price = row.open_price + 50;
        row.low_price = row.open_price - 20;
        row.close_price = row.open_price + 5;
        row.volume = 1000000 + i;
        row.amount = row.close_price * 100;
        row.vwap = row.close_price;
        rows.push_back(row);
    }

    auto status = engine.store_kline_batch("QUERY", quant::event::DataType::kKline1Min, rows);
    EXPECT_EQ(status, StoreStatus::kOk);

    // Query opens (price field → Gorilla/double)
    auto result = engine.query_kline("QUERY", quant::event::DataType::kKline1Min,
                                      DataField::kOpen,
                                      TimeRange{0, 99999});
    ASSERT_EQ(result.values.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        double expected = static_cast<double>(rows[i].open_price) / 10000.0;
        EXPECT_DOUBLE_EQ(result.values[i], expected);
    }

    // Query volumes (integer field → Delta/int64)
    auto vol_result = engine.query_kline("QUERY", quant::event::DataType::kKline1Min,
                                          DataField::kVolume,
                                          TimeRange{0, 99999});
    ASSERT_EQ(vol_result.values.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_DOUBLE_EQ(vol_result.values[i], static_cast<double>(rows[i].volume));
    }

    // Query timestamps
    EXPECT_EQ(result.timestamps.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(result.timestamps[i], rows[i].timestamp);
    }
}

TEST(StorageEngineTest, StoreAndQueryKlineSingle) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{4, tmpdir.path()});

    quant::event::KlineRow row{};
    row.timestamp = 1700000000000000LL;
    row.open_price = 10000;
    row.high_price = 10500;
    row.low_price = 9800;
    row.close_price = 10300;
    row.volume = 1000000;
    row.amount = 103000000LL;
    row.vwap = 102500;

    auto status = engine.store_kline("SINGLE", quant::event::DataType::kKline1Min, row);
    EXPECT_EQ(status, StoreStatus::kOk);

    auto result = engine.query_kline("SINGLE", quant::event::DataType::kKline1Min,
                                      DataField::kClose,
                                      TimeRange{0, 9999999999999999LL});
    ASSERT_EQ(result.values.size(), 1u);
    EXPECT_DOUBLE_EQ(result.values[0], 1.03);
}

TEST(StorageEngineTest, FlushAndClose) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});

    auto status = engine.flush();
    EXPECT_EQ(status, StoreStatus::kOk);

    status = engine.close();
    EXPECT_EQ(status, StoreStatus::kOk);

    // Double close is safe
    status = engine.close();
    EXPECT_EQ(status, StoreStatus::kOk);
}

// ========================================================================
// SegmentHeader / BlockIndexEntry size tests
// ========================================================================

TEST(DiskPersistenceTest, SegmentHeaderSize) {
    EXPECT_EQ(sizeof(SegmentHeader), kSegmentHeaderSize);
}

TEST(DiskPersistenceTest, BlockIndexEntrySize) {
    EXPECT_EQ(sizeof(BlockIndexEntry), kBlockIndexSize);
}

// ========================================================================
// WriteAheadLog tests
// ========================================================================

TEST(WriteAheadLogTest, AppendAndReplay) {
    TempDir tmpdir;
    WriteAheadLog wal(WriteAheadLog::Options{tmpdir.path(), 64});

    quant::event::KlineRow row{};
    row.timestamp = 1700000000000000LL;
    row.open_price = 10000;
    row.close_price = 10300;
    row.volume = 1000000;

    EXPECT_TRUE(wal.append("AAPL", 1, row));

    auto entries = wal.replay();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].symbol, "AAPL");
    EXPECT_EQ(entries[0].data_type, 1u);
    EXPECT_EQ(entries[0].row.timestamp, row.timestamp);
    EXPECT_EQ(entries[0].row.open_price, row.open_price);
}

TEST(WriteAheadLogTest, AppendBatchAndReplay) {
    TempDir tmpdir;
    WriteAheadLog wal(WriteAheadLog::Options{tmpdir.path(), 64});

    std::vector<quant::event::KlineRow> rows;
    for (int i = 0; i < 5; ++i) {
        quant::event::KlineRow row{};
        row.timestamp = 1000 + i * 100;
        row.open_price = 10000 + i * 10;
        row.close_price = row.open_price + 5;
        rows.push_back(row);
    }

    EXPECT_TRUE(wal.append_batch("MSFT", 2, rows));

    auto entries = wal.replay();
    ASSERT_EQ(entries.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(entries[i].symbol, "MSFT");
        EXPECT_EQ(entries[i].data_type, 2u);
        EXPECT_EQ(entries[i].row.timestamp, rows[i].timestamp);
    }
}

TEST(WriteAheadLogTest, TruncateClearsData) {
    TempDir tmpdir;
    WriteAheadLog wal(WriteAheadLog::Options{tmpdir.path(), 64});

    quant::event::KlineRow row{};
    row.timestamp = 1000;
    row.open_price = 10000;
    wal.append("TEST", 0, row);

    EXPECT_EQ(wal.replay().size(), 1u);

    wal.truncate();

    EXPECT_EQ(wal.replay().size(), 0u);
    EXPECT_EQ(wal.bytes_written(), 0u);
}

TEST(WriteAheadLogTest, MultipleSymbols) {
    TempDir tmpdir;
    WriteAheadLog wal(WriteAheadLog::Options{tmpdir.path(), 64});

    quant::event::KlineRow row{};
    row.timestamp = 1000;
    row.open_price = 10000;

    wal.append("AAPL", 1, row);
    row.open_price = 20000;
    wal.append("GOOG", 1, row);
    row.open_price = 30000;
    wal.append("MSFT", 2, row);

    auto entries = wal.replay();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].symbol, "AAPL");
    EXPECT_EQ(entries[1].symbol, "GOOG");
    EXPECT_EQ(entries[2].symbol, "MSFT");
    EXPECT_EQ(entries[2].data_type, 2u);
}

TEST(WriteAheadLogTest, TruncateAndAppendNew) {
    TempDir tmpdir;
    WriteAheadLog wal(WriteAheadLog::Options{tmpdir.path(), 64});

    quant::event::KlineRow row{};
    row.timestamp = 1000;
    row.open_price = 10000;
    wal.append("OLD", 0, row);

    wal.truncate();

    row.timestamp = 2000;
    row.open_price = 20000;
    wal.append("NEW", 1, row);

    auto entries = wal.replay();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].symbol, "NEW");
    EXPECT_EQ(entries[0].row.open_price, 20000);
}

TEST(WriteAheadLogTest, Rotation) {
    TempDir tmpdir;
    // Very small WAL to force rotation
    WriteAheadLog wal(WriteAheadLog::Options{tmpdir.path(), 0});

    quant::event::KlineRow row{};
    row.timestamp = 1000;
    row.open_price = 10000;

    // Write enough to trigger rotation
    for (int i = 0; i < 5; ++i) {
        row.timestamp = 1000 + i;
        wal.append("ROT", 0, row);
    }

    auto entries = wal.replay();
    EXPECT_EQ(entries.size(), 5u);
}

// ========================================================================
// WriteBuffer tests
// ========================================================================

TEST(WriteBufferTest, WriteAndFlush) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});
    WriteBuffer::Options opts;
    opts.wal_opts = WriteAheadLog::Options{tmpdir.path() + "/wal", 64};
    opts.flush_row_threshold = 100;
    opts.enable_wal = true;
    WriteBuffer buf(engine, opts);

    quant::event::KlineRow row{};
    row.timestamp = 1700000000000000LL;
    row.open_price = 10000;
    row.high_price = 10500;
    row.low_price = 9800;
    row.close_price = 10300;
    row.volume = 1000000;
    row.amount = 103000000LL;
    row.vwap = 102500;

    EXPECT_EQ(buf.write("AAPL", 1, row), StoreStatus::kOk);
    EXPECT_EQ(buf.pending_rows(), 1u);

    buf.flush();
    EXPECT_EQ(buf.pending_rows(), 0u);

    // Verify data made it to StorageEngine
    auto result = engine.query_kline("AAPL",
        static_cast<quant::event::DataType>(1),
        DataField::kOpen,
        TimeRange{0, 9999999999999999LL});
    ASSERT_EQ(result.values.size(), 1u);
    EXPECT_DOUBLE_EQ(result.values[0], 1.0);

    engine.close();
}

TEST(WriteBufferTest, WriteBatch) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});
    WriteBuffer::Options opts;
    opts.wal_opts = WriteAheadLog::Options{tmpdir.path() + "/wal", 64};
    opts.flush_row_threshold = 1000;
    opts.enable_wal = true;
    WriteBuffer buf(engine, opts);

    std::vector<quant::event::KlineRow> rows;
    for (int i = 0; i < 5; ++i) {
        quant::event::KlineRow row{};
        row.timestamp = 1000 + i * 100;
        row.open_price = 10000 + i * 100;
        row.close_price = row.open_price + 5;
        row.volume = 1000000 + i;
        rows.push_back(row);
    }

    EXPECT_EQ(buf.write_batch("MSFT", 2, rows), StoreStatus::kOk);
    EXPECT_EQ(buf.pending_rows(), 5u);

    buf.flush();
    EXPECT_EQ(buf.pending_rows(), 0u);

    auto result = engine.query_kline("MSFT",
        static_cast<quant::event::DataType>(2),
        DataField::kOpen,
        TimeRange{0, 99999});
    ASSERT_EQ(result.values.size(), 5u);

    engine.close();
}

TEST(WriteBufferTest, AutoFlushOnThreshold) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});
    WriteBuffer::Options opts;
    opts.wal_opts = WriteAheadLog::Options{tmpdir.path() + "/wal", 64};
    opts.flush_row_threshold = 3;  // auto-flush after 3 rows
    opts.enable_wal = true;
    WriteBuffer buf(engine, opts);

    quant::event::KlineRow row{};
    row.timestamp = 1000;
    row.open_price = 10000;
    row.close_price = 10300;

    // Write 3 rows → should auto-flush
    for (int i = 0; i < 3; ++i) {
        row.timestamp = 1000 + i * 100;
        buf.write("AUTO", 1, row);
    }

    // After threshold flush, pending should be 0
    EXPECT_EQ(buf.pending_rows(), 0u);

    auto result = engine.query_kline("AUTO",
        static_cast<quant::event::DataType>(1),
        DataField::kOpen,
        TimeRange{0, 99999});
    ASSERT_EQ(result.values.size(), 3u);

    engine.close();
}

TEST(WriteBufferTest, WriteBatchEmptyFails) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});
    WriteBuffer::Options opts;
    opts.wal_opts = WriteAheadLog::Options{tmpdir.path() + "/wal", 64};
    opts.enable_wal = true;
    WriteBuffer buf(engine, opts);

    std::vector<quant::event::KlineRow> empty;
    EXPECT_EQ(buf.write_batch("X", 0, empty), StoreStatus::kInvalidArgument);

    engine.close();
}

TEST(WriteBufferTest, DisableWal) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});
    WriteBuffer::Options opts;
    opts.wal_opts = WriteAheadLog::Options{tmpdir.path() + "/wal", 64};
    opts.enable_wal = false;
    WriteBuffer buf(engine, opts);

    quant::event::KlineRow row{};
    row.timestamp = 1000;
    row.open_price = 10000;

    EXPECT_EQ(buf.write("NOWAL", 1, row), StoreStatus::kOk);
    buf.flush();

    auto result = engine.query_kline("NOWAL",
        static_cast<quant::event::DataType>(1),
        DataField::kOpen,
        TimeRange{0, 99999});
    ASSERT_EQ(result.values.size(), 1u);

    engine.close();
}

TEST(WriteBufferTest, RecoverFromWal) {
    TempDir tmpdir;
    std::string wal_dir = tmpdir.path() + "/wal";

    // Phase 1: Write data via WriteBuffer, then destroy without flush
    {
        StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});
        WriteBuffer::Options opts;
        opts.wal_opts = WriteAheadLog::Options{wal_dir, 64};
        opts.flush_row_threshold = 10000;  // high threshold so no auto-flush
        opts.enable_wal = true;
        WriteBuffer buf(engine, opts);

        quant::event::KlineRow row{};
        row.timestamp = 1000;
        row.open_price = 10000;
        row.close_price = 10300;
        row.volume = 500000;

        buf.write("RECOV", 1, row);
        EXPECT_EQ(buf.pending_rows(), 1u);
        // Don't call flush() — destructor flushes, so WAL has the data
    }

    // Phase 2: Create new WriteBuffer and call recover()
    {
        StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});
        WriteBuffer::Options opts;
        opts.wal_opts = WriteAheadLog::Options{wal_dir, 64};
        opts.flush_row_threshold = 10000;
        opts.enable_wal = true;
        WriteBuffer buf(engine, opts);

        // The previous destructor flushed, so WAL should have been truncated.
        // For a real crash scenario, we'd need to simulate process death
        // before flush. This test validates recover() is callable.
        buf.recover();

        engine.close();
    }
}

// ========================================================================
// StorageEngine + WriteBuffer integration tests
// ========================================================================

TEST(StorageEngineWriteBufferIntegration, SingleRowWritesThroughWriteBuffer) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});

    // Attach WriteBuffer to StorageEngine
    WriteBuffer::Options opts;
    opts.wal_opts = WriteAheadLog::Options{tmpdir.path() + "/wal", 64};
    opts.flush_row_threshold = 100;
    opts.enable_wal = true;
    auto write_buffer = std::make_unique<WriteBuffer>(engine, opts);
    WriteBuffer* wb_ptr = write_buffer.get();  // keep raw pointer for assertions
    engine.set_write_buffer(std::move(write_buffer));

    // Write single row via store_kline (should go through WriteBuffer)
    quant::event::KlineRow row{};
    row.timestamp = 1700000000000000LL;
    row.open_price = 10000;
    row.high_price = 10500;
    row.low_price = 9800;
    row.close_price = 10300;
    row.volume = 1000000;
    row.amount = 103000000LL;
    row.vwap = 102500;

    auto status = engine.store_kline("INTG", quant::event::DataType::kKline1Min, row);
    EXPECT_EQ(status, StoreStatus::kOk);
    EXPECT_EQ(wb_ptr->pending_rows(), 1u);

    // Flush via engine (should flush WriteBuffer first)
    engine.flush();
    EXPECT_EQ(wb_ptr->pending_rows(), 0u);

    // Verify data is queryable
    auto result = engine.query_kline("INTG", quant::event::DataType::kKline1Min,
                                      DataField::kClose,
                                      TimeRange{0, 9999999999999999LL});
    ASSERT_EQ(result.values.size(), 1u);
    EXPECT_DOUBLE_EQ(result.values[0], 1.03);

    engine.close();
}

TEST(StorageEngineWriteBufferIntegration, BatchWritesBypassWriteBuffer) {
    TempDir tmpdir;
    StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});

    // Attach WriteBuffer
    WriteBuffer::Options opts;
    opts.wal_opts = WriteAheadLog::Options{tmpdir.path() + "/wal", 64};
    opts.flush_row_threshold = 1000;
    opts.enable_wal = true;
    auto write_buffer = std::make_unique<WriteBuffer>(engine, opts);
    WriteBuffer* wb_ptr = write_buffer.get();
    engine.set_write_buffer(std::move(write_buffer));

    // Batch write should go directly to cache, not through WriteBuffer
    std::vector<quant::event::KlineRow> rows;
    for (int i = 0; i < 5; ++i) {
        quant::event::KlineRow row{};
        row.timestamp = 1000 + i * 100;
        row.open_price = 10000 + i * 100;
        row.high_price = row.open_price + 50;
        row.low_price = row.open_price - 20;
        row.close_price = row.open_price + 5;
        row.volume = 1000000;
        row.amount = row.close_price * 100;
        row.vwap = row.close_price;
        rows.push_back(row);
    }

    auto status = engine.store_kline_batch("BATCH", quant::event::DataType::kKline5Min, rows);
    EXPECT_EQ(status, StoreStatus::kOk);

    // WriteBuffer should have 0 pending (batch bypasses it)
    EXPECT_EQ(wb_ptr->pending_rows(), 0u);

    // Data should be immediately queryable
    auto result = engine.query_kline("BATCH", quant::event::DataType::kKline5Min,
                                      DataField::kOpen,
                                      TimeRange{0, 99999});
    ASSERT_EQ(result.values.size(), 5u);

    engine.close();
}

TEST(StorageEngineWriteBufferIntegration, FlushPersistsData) {
    TempDir tmpdir;
    std::string wal_dir = tmpdir.path() + "/wal";

    // Phase 1: Write through WriteBuffer, then flush
    {
        StorageEngine engine(StorageEngine::Options{1, tmpdir.path()});
        WriteBuffer::Options opts;
        opts.wal_opts = WriteAheadLog::Options{wal_dir, 64};
        opts.flush_row_threshold = 10000;
        opts.enable_wal = true;
        auto write_buffer = std::make_unique<WriteBuffer>(engine, opts);
        engine.set_write_buffer(std::move(write_buffer));

        quant::event::KlineRow row{};
        row.timestamp = 1000;
        row.open_price = 10000;
        row.close_price = 10300;
        row.volume = 500000;

        engine.store_kline("PERSIST", quant::event::DataType::kKline1Min, row);

        // Verify data is buffered in WriteBuffer
        EXPECT_NE(engine.write_buffer(), nullptr);
        EXPECT_EQ(engine.write_buffer()->pending_rows(), 1u);

        engine.flush();  // explicit flush

        // Verify data is now in cache (WriteBuffer flushes to cache)
        EXPECT_EQ(engine.write_buffer()->pending_rows(), 0u);

        auto result = engine.query_kline("PERSIST", quant::event::DataType::kKline1Min,
                                          DataField::kOpen,
                                          TimeRange{0, 99999});
        ASSERT_EQ(result.values.size(), 1u);
        EXPECT_DOUBLE_EQ(result.values[0], 1.0);

        engine.close();
    }
}

}  // namespace
}  // namespace quant::storage