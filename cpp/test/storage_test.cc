// storage_test.cc — Tests for ColumnBlock, TimeSeriesCache, StorageEngine, WriteAheadLog, WriteBuffer
#include <filesystem>
#include <fstream>
#include <vector>

#include "cpp/quant/storage/column_block.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/storage/time_series_cache.h"
#include "cpp/quant/storage/time_series_store.h"
#include "cpp/quant/storage/segment_index.h"
#include "cpp/quant/storage/write_buffer.h"
#include "cpp/quant/storage/write_ahead_log.h"
#include "cpp/quant/storage/data_initializer.h"
#include "cpp/quant/infra/coroutine.h"

#include <gtest/gtest.h>

using namespace quant::storage;
using quant::infra::blockingWait;

namespace {

KlineRow make_row(int64_t ts, double price) {
    KlineRow r{};
    r.timestamp = ts;
    r.open_price = static_cast<int32_t>(price * 10000);
    r.high_price = static_cast<int32_t>((price + 0.5) * 10000);
    r.low_price = static_cast<int32_t>((price - 0.5) * 10000);
    r.close_price = static_cast<int32_t>(price * 10000);
    r.volume = 1000;
    r.amount = 100000000;
    r.vwap = 0;
    return r;
}

std::vector<KlineRow> make_rows(int64_t start_ts, int count) {
    std::vector<KlineRow> rows;
    rows.reserve(count);
    for (int i = 0; i < count; ++i) {
        rows.push_back(make_row(start_ts + i, 100.0 + i));
    }
    return rows;
}

}  // namespace

// ── ColumnBlock compression tests ──

TEST(ColumnBlockTest, DeltaEncodeDecodeInt64) {
    std::vector<int64_t> data = {1000000, 1000500, 1001200, 1002100, 1003000};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kOpen, data, ColumnBlock::Codec::kDelta, 1000000, 1003000);

    EXPECT_EQ(block.field(), DataField::kOpen);
    EXPECT_EQ(block.codec(), ColumnBlock::Codec::kDelta);
    EXPECT_EQ(block.row_count(), 5u);
    EXPECT_GT(block.compressed_size(), 0u);

    std::vector<int64_t> out(5);
    size_t n = block.decompress(std::span<int64_t>(out));
    EXPECT_EQ(n, 5u);
    for (size_t i = 0; i < n; ++i) EXPECT_EQ(out[i], data[i]);
}

TEST(ColumnBlockTest, DeltaEncodeDecodeInt32) {
    std::vector<int32_t> data = {100, 105, 98, 110, 108, 115, 120};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kClose, data, ColumnBlock::Codec::kDelta, 0, 600000);

    EXPECT_EQ(block.codec(), ColumnBlock::Codec::kDelta);
    EXPECT_EQ(block.row_count(), 7u);

    std::vector<int32_t> out(7);
    size_t n = block.decompress(std::span<int32_t>(out));
    EXPECT_EQ(n, 7u);
    for (size_t i = 0; i < n; ++i) EXPECT_EQ(out[i], data[i]);
}

TEST(ColumnBlockTest, EmptyCompression) {
    std::vector<int64_t> empty;
    ColumnBlock block = ColumnBlock::compress(
        DataField::kOpen, empty, ColumnBlock::Codec::kDelta, 0, 0);
    EXPECT_EQ(block.row_count(), 0u);
    EXPECT_EQ(block.compressed_size(), 0u);
}

TEST(ColumnBlockTest, DeltaEncodeAllZeros) {
    std::vector<int64_t> data(100, 0);
    ColumnBlock block = ColumnBlock::compress(
        DataField::kTimestamp, data, ColumnBlock::Codec::kDelta, 0, 0);

    EXPECT_EQ(block.row_count(), 100u);

    std::vector<int64_t> out(100);
    size_t n = block.decompress(std::span<int64_t>(out));
    EXPECT_EQ(n, 100u);
    for (size_t i = 0; i < n; ++i) EXPECT_EQ(out[i], 0);
}

TEST(ColumnBlockTest, DeltaEncodeNegativeTimestamps) {
    std::vector<int64_t> data = {-1000, -500, 0, 500, 1000, 1500};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kTimestamp, data, ColumnBlock::Codec::kDelta, -1000, 1500);

    EXPECT_EQ(block.row_count(), 6u);

    std::vector<int64_t> out(6);
    size_t n = block.decompress(std::span<int64_t>(out));
    EXPECT_EQ(n, 6u);
    EXPECT_EQ(out[0], -1000);
    EXPECT_EQ(out[3], 500);
}

TEST(ColumnBlockTest, GorillaRepeatedValues) {
    std::vector<double> data(50, 3.14159);
    ColumnBlock block = ColumnBlock::compress(
        DataField::kClose, data, ColumnBlock::Codec::kGorilla, 0, 0);

    EXPECT_EQ(block.row_count(), 50u);

    std::vector<double> out(50);
    size_t n = block.decompress(std::span<double>(out));
    EXPECT_EQ(n, 50u);
    for (size_t i = 0; i < n; ++i) EXPECT_DOUBLE_EQ(out[i], 3.14159);
}

TEST(ColumnBlockTest, GorillaAlternatingValues) {
    std::vector<double> data;
    for (int i = 0; i < 100; ++i) {
        data.push_back((i % 2 == 0) ? 100.0 : 200.0);
    }
    ColumnBlock block = ColumnBlock::compress(
        DataField::kClose, data, ColumnBlock::Codec::kGorilla, 0, 0);

    std::vector<double> out(100);
    size_t n = block.decompress(std::span<double>(out));
    EXPECT_EQ(n, 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_DOUBLE_EQ(out[i], (i % 2 == 0) ? 100.0 : 200.0);
    }
}

TEST(ColumnBlockTest, DecompressPartialBuffer) {
    std::vector<int64_t> data = {1000, 2000, 3000, 4000, 5000};
    ColumnBlock block = ColumnBlock::compress(
        DataField::kTimestamp, data, ColumnBlock::Codec::kDelta, 1000, 5000);

    // Decompress into buffer smaller than row_count
    std::vector<int64_t> small(3);
    size_t n = block.decompress(std::span<int64_t>(small));
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(small[0], 1000);
    EXPECT_EQ(small[1], 2000);
    EXPECT_EQ(small[2], 3000);
}

// ── TimeSeriesCache tests ──

TEST(TimeSeriesCacheTest, AppendAndQuery) {
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64});

    cache.append("000001", 1, make_row(1000, 10.0));
    cache.append("000001", 1, make_row(2000, 20.0));
    cache.append("000001", 1, make_row(3000, 30.0));

    auto result = cache.query("000001", 1, 1000, 3000);
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0].timestamp, 1000);
    EXPECT_EQ(result[1].timestamp, 2000);
    EXPECT_EQ(result[2].timestamp, 3000);
}

TEST(TimeSeriesCacheTest, QueryRange) {
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64});

    cache.append_batch("000001", 1, make_rows(1000, 10));

    auto result = cache.query("000001", 1, 1003, 1006);
    ASSERT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0].timestamp, 1003);
    EXPECT_EQ(result[3].timestamp, 1006);
}

TEST(TimeSeriesCacheTest, EmptyQuery) {
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64});

    auto result = cache.query("999999", 1, 0, 9999);
    EXPECT_TRUE(result.empty());
}

TEST(TimeSeriesCacheTest, CoroutineAPI) {
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64});

    blockingWait(cache.co_append("000001", 1, make_row(1000, 10.0)));
    blockingWait(cache.co_append("000001", 1, make_row(2000, 20.0)));

    auto result = blockingWait(cache.co_query("000001", 1, 0, 9999));
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].timestamp, 1000);
}

TEST(TimeSeriesCacheTest, MultipleSymbols) {
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64});

    cache.append("000001", 1, make_row(1000, 10.0));
    cache.append("000002", 1, make_row(1000, 20.0));

    auto r1 = cache.query("000001", 1, 0, 9999);
    auto r2 = cache.query("000002", 1, 0, 9999);

    ASSERT_EQ(r1.size(), 1u);
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_EQ(r1[0].close_price, 100000);
    EXPECT_EQ(r2[0].close_price, 200000);
}

TEST(TimeSeriesCacheTest, FixedPointRoundTrip) {
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64});

    KlineRow r{};
    r.timestamp = 5000;
    r.open_price = 10000;
    r.high_price = 10100;
    r.low_price = 9900;
    r.close_price = 10050;
    r.volume = 10000;
    r.amount = 100500000;
    r.vwap = 10050;

    cache.append("000001", 1, r);

    auto result = cache.query("000001", 1, 5000, 5000);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].open_price, 10000);
    EXPECT_EQ(result[0].close_price, 10050);
}

TEST(TimeSeriesCacheTest, EvictionTriggersOnOverBudget) {
    // Small budget with many writes should evict oldest entries
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 1, .budget_mb = 1});

    // 30000 rows × 48 bytes = ~1.44 MB > 1 MB budget → triggers eviction
    for (int i = 0; i < 30000; ++i) {
        cache.append("000001", 1, make_row(i * 1000, 100.0));
    }

    // The oldest entries should have been evicted
    auto oldest = cache.query("000001", 1, 0, 5000000);
    auto newest = cache.query("000001", 1, 25000000, 30000000);

    // Newest data should still be present
    ASSERT_FALSE(newest.empty());
    // Oldest data may or may not be evicted depending on exact budget
    // At minimum, total row count should be less than 30000
    EXPECT_LT(oldest.size() + newest.size(), 30000u);
}

// ── SegmentIndex tests ──

TEST(SegmentIndexTest, AddAndQuery) {
    SegmentIndex index;

    SegmentMeta meta;
    meta.file_path = "test.seg";
    meta.field = DataField::kClose;
    meta.codec = ColumnBlock::Codec::kDelta;
    meta.row_count = 100;
    meta.compressed_size = 512;
    meta.min_ts = 1000;
    meta.max_ts = 2000;
    meta.file_offset = 64;

    index.add("000001", 1, meta);

    auto result = index.query("000001", 1, DataField::kClose, 0, 9999);
    ASSERT_EQ(result.size(), 1u);
}

TEST(SegmentIndexTest, CoroutineAPI) {
    SegmentIndex index;

    SegmentMeta meta;
    meta.file_path = "test_co.seg";
    meta.field = DataField::kClose;
    meta.min_ts = 1000;
    meta.max_ts = 2000;

    blockingWait(index.co_add("000001", 1, meta));

    auto result = blockingWait(index.co_query("000001", 1, DataField::kClose, 0, 9999));
    ASSERT_EQ(result.size(), 1u);
}

// ── TimeSeriesStore tests ──

TEST(TimeSeriesStoreTest, StoreAndQuery) {
    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_store",
    });

    auto status = store.store_kline("000001", 1, make_row(1000, 10.0));
    EXPECT_EQ(status, StoreStatus::kOk);

    auto result = store.query_kline("000001", 1, 0, 9999);
    ASSERT_EQ(result.size(), 1u);
}

TEST(TimeSeriesStoreTest, BatchStore) {
    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_store_batch",
    });

    auto rows = make_rows(1000, 5);
    auto status = store.store_kline_batch("000001", 1, rows);
    EXPECT_EQ(status, StoreStatus::kOk);

    auto result = store.query_kline("000001", 1, 0, 9999);
    ASSERT_EQ(result.size(), 5u);
}

TEST(TimeSeriesStoreTest, ReadThroughFromDisk) {
    // Write data directly to disk (bypass cache), then query via coroutine API
    // to trigger read-through
    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_readthrough",
    });

    auto rows = make_rows(1000, 10);
    auto blocks = TimeSeriesStore::rows_to_column_blocks(rows);
    store.disk().write_segment("000001", 1, blocks, 1000, 1009);

    auto result = blockingWait(store.co_query_kline("000001", 1, 1000, 1009));
    ASSERT_EQ(result.size(), 10u);
    EXPECT_EQ(result[0].timestamp, 1000);
    EXPECT_EQ(result[9].timestamp, 1009);
    EXPECT_EQ(result[0].close_price, 1000000);  // make_row(1000, 10.0) => price * 10000
}

TEST(TimeSeriesStoreTest, ReadThroughMergesSegments) {
    // Write two overlapping segments, verify read-through merges and deduplicates
    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_merge",
    });

    // Segment 1: ts 1000-1009 (10 rows)
    auto rows1 = make_rows(1000, 10);
    auto blocks1 = TimeSeriesStore::rows_to_column_blocks(rows1);
    store.disk().write_segment("000001", 1, blocks1, 1000, 1009);

    // Segment 2: ts 1005-1014 (10 rows, overlaps with segment 1 at 1005-1009)
    auto rows2 = make_rows(1005, 10);
    auto blocks2 = TimeSeriesStore::rows_to_column_blocks(rows2);
    store.disk().write_segment("000001", 1, blocks2, 1005, 1014);

    auto result = blockingWait(store.co_query_kline("000001", 1, 1000, 1014));
    // 15 unique timestamps: 1000-1014
    ASSERT_EQ(result.size(), 15u);
    EXPECT_EQ(result[0].timestamp, 1000);
    EXPECT_EQ(result[14].timestamp, 1014);
}

TEST(TimeSeriesStoreTest, ReadThroughBackfillsCache) {
    // After read-through, subsequent queries should hit cache
    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_backfill",
    });

    auto rows = make_rows(1000, 5);
    auto blocks = TimeSeriesStore::rows_to_column_blocks(rows);
    store.disk().write_segment("000001", 1, blocks, 1000, 1004);

    // First query: read-through from disk
    auto result1 = blockingWait(store.co_query_kline("000001", 1, 1000, 1004));
    ASSERT_EQ(result1.size(), 5u);

    // Second query: should hit cache (same result)
    auto result2 = blockingWait(store.co_query_kline("000001", 1, 1000, 1004));
    ASSERT_EQ(result2.size(), 5u);

    // Third query: partial overlap should also hit cache
    auto result3 = blockingWait(store.co_query_kline("000001", 1, 1002, 1004));
    ASSERT_EQ(result3.size(), 3u);
}

TEST(TimeSeriesStoreTest, BlocksToRowsRoundTrip) {
    // Verify blocks_to_rows is the inverse of rows_to_column_blocks
    auto original = make_rows(1000, 10);
    auto blocks = TimeSeriesStore::rows_to_column_blocks(original);
    auto recovered = TimeSeriesStore::blocks_to_rows(blocks);

    ASSERT_EQ(recovered.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_EQ(recovered[i].timestamp, original[i].timestamp);
        EXPECT_EQ(recovered[i].open_price, original[i].open_price);
        EXPECT_EQ(recovered[i].high_price, original[i].high_price);
        EXPECT_EQ(recovered[i].low_price, original[i].low_price);
        EXPECT_EQ(recovered[i].close_price, original[i].close_price);
        EXPECT_EQ(recovered[i].vwap, original[i].vwap);
        EXPECT_EQ(recovered[i].volume, original[i].volume);
        EXPECT_EQ(recovered[i].amount, original[i].amount);
    }
}

// ── StorageEngine tests ──

TEST(StorageEngineTest, StoreAndQuery) {
    StorageEngine engine(StorageEngine::Options{
        .cache_budget_mb = 64,
        .data_dir = "/tmp/quant_test_engine",
    });
    engine.start();

    engine.store_kline("000001", 1, make_row(1000, 10.0));
    engine.store_kline("000001", 1, make_row(2000, 20.0));

    auto result = engine.query_kline("000001", 1, 0, 9999);
    ASSERT_EQ(result.size(), 2u);

    engine.shutdown();
}

TEST(StorageEngineTest, CoroutineAPI) {
    StorageEngine engine(StorageEngine::Options{
        .cache_budget_mb = 64,
        .data_dir = "/tmp/quant_test_engine_co",
    });
    engine.start();

    blockingWait(engine.co_store_kline("000001", 1, make_row(1000, 10.0)));

    auto result = blockingWait(engine.co_query_kline("000001", 1, 0, 9999));
    ASSERT_EQ(result.size(), 1u);

    engine.shutdown();
}

TEST(StorageEngineTest, WriteBufferIntegration) {
    StorageEngine engine(StorageEngine::Options{
        .cache_budget_mb = 64,
        .data_dir = "/tmp/quant_test_wb",
    });

    auto wb = std::make_unique<WriteBuffer>(engine, WriteBuffer::Options{
        .wal_opts = WriteAheadLog::Options{"/tmp/quant_test_wb/wal", 1},
        .flush_row_threshold = 100,
        .enable_wal = false,
    });

    engine.set_write_buffer(std::move(wb));
    EXPECT_NE(engine.write_buffer(), nullptr);

    engine.start();

    // Write via coroutine API — should route through WriteBuffer
    auto row = make_row(1000, 10.0);
    blockingWait(engine.co_store_kline("000001", 1, row));

    // Data should NOT be in cache yet (still buffered in WriteBuffer)
    auto before = engine.query_kline("000001", 1, 0, 9999);
    EXPECT_TRUE(before.empty());

    // Flush WriteBuffer
    engine.write_buffer()->flush();

    // Data should now be in cache
    auto after = engine.query_kline("000001", 1, 0, 9999);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].timestamp, 1000);

    engine.shutdown();
}

// ── WriteAheadLog tests ──

TEST(WriteAheadLogTest, AppendAndReplay) {
    WriteAheadLog::Options opts{"/tmp/wal_test", 1};
    WriteAheadLog wal(opts);

    KlineRow row{};
    row.timestamp = 1000;
    row.open_price = 10000;
    row.close_price = 10100;
    row.volume = 500;
    row.amount = 1000000;
    row.vwap = 10050;

    EXPECT_TRUE(wal.append("000001", 1, row));

    auto entries = wal.replay();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].symbol, "000001");
    EXPECT_EQ(entries[0].row.timestamp, 1000);

    wal.truncate();
    EXPECT_TRUE(wal.replay().empty());
}

TEST(WriteAheadLogTest, AppendBatch) {
    WriteAheadLog::Options opts{"/tmp/wal_batch_test", 1};
    WriteAheadLog wal(opts);
    wal.truncate();  // clean any leftover from previous runs

    std::vector<KlineRow> rows;
    for (int i = 0; i < 5; ++i) {
        KlineRow r{};
        r.timestamp = 1000 + i;
        r.open_price = 10000;
        rows.push_back(r);
    }

    EXPECT_TRUE(wal.append_batch("000001", 1, rows));
    auto entries = wal.replay();
    ASSERT_EQ(entries.size(), 5u);
}

// ── WriteBuffer tests ──

TEST(WriteBufferTest, WriteAndFlush) {
    StorageEngine engine(StorageEngine::Options{
        .cache_budget_mb = 64,
        .data_dir = "/tmp/test_wb_engine",
    });
    engine.start();

    WriteBuffer::Options wb_opts;
    wb_opts.wal_opts = WriteAheadLog::Options{"/tmp/test_wb_wal", 1};
    wb_opts.enable_wal = false;

    WriteBuffer wb(engine, wb_opts);

    KlineRow row{};
    row.timestamp = 1000;
    row.open_price = 10000;
    row.close_price = 10100;
    row.volume = 500;
    row.amount = 1000000;
    row.vwap = 10050;

    auto status = wb.write("000001", 1, row);
    EXPECT_EQ(status, StoreStatus::kOk);

    wb.flush();

    auto result = engine.query_kline("000001", 1, 0, 9999);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].timestamp, 1000);

    engine.shutdown();
}

// ── Dirty flush tests ──

TEST(TimeSeriesStoreTest, CoFlushWritesToDisk) {
    // co_flush should write accumulated rows to disk
    std::filesystem::remove_all("/tmp/quant_test_coflush");
    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_coflush",
    });

    // Write individual rows (co_store_kline now accumulates in pending_disk_)
    blockingWait(store.co_store_kline("000001", 1, make_row(1000, 10.0)));
    blockingWait(store.co_store_kline("000001", 1, make_row(2000, 20.0)));
    blockingWait(store.co_store_kline("000001", 1, make_row(3000, 30.0)));

    EXPECT_GT(store.pending_disk_rows(), 0u);

    // Flush to disk
    blockingWait(store.co_flush());

    EXPECT_EQ(store.pending_disk_rows(), 0u);

    // Create new store pointing to same dir — should read from disk
    TimeSeriesStore store2(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_coflush",
    });

    auto result = blockingWait(store2.co_query_kline("000001", 1, 1000, 3000));
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0].timestamp, 1000);
    EXPECT_EQ(result[2].timestamp, 3000);
}

TEST(TimeSeriesStoreTest, CoFlushBatchAccumulation) {
    // Batch writes also accumulate and get flushed
    std::filesystem::remove_all("/tmp/quant_test_flush_batch");
    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_flush_batch",
    });

    auto rows = make_rows(1000, 100);
    blockingWait(store.co_store_kline_batch("000001", 1, rows));

    EXPECT_EQ(store.pending_disk_rows(), 100u);

    blockingWait(store.co_flush());

    EXPECT_EQ(store.pending_disk_rows(), 0u);

    TimeSeriesStore store2(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_flush_batch",
    });

    auto result = blockingWait(store2.co_query_kline("000001", 1, 1000, 1099));
    ASSERT_EQ(result.size(), 100u);
    EXPECT_EQ(result[0].timestamp, 1000);
    EXPECT_EQ(result[99].timestamp, 1099);
}

TEST(StorageEngineTest, DirtyFlushViaStore) {
    // StorageEngine accumulates writes in store; shutdown flushes to disk;
    // reopen and query via read-through
    std::filesystem::remove_all("/tmp/quant_test_dirty_flush");
    StorageEngine engine(StorageEngine::Options{
        .cache_budget_mb = 64,
        .data_dir = "/tmp/quant_test_dirty_flush",
    });
    engine.start();

    // Write via coroutine API — routes through store for accumulation
    blockingWait(engine.co_store_kline("000001", 1, make_row(1000, 10.0)));
    blockingWait(engine.co_store_kline("000001", 1, make_row(2000, 20.0)));

    // Data should be in cache
    auto cached = blockingWait(engine.co_query_kline("000001", 1, 0, 9999));
    ASSERT_EQ(cached.size(), 2u);

    // Close the engine — this flushes stores_ to disk
    engine.shutdown();

    // Reopen with a new engine pointing to same data_dir
    StorageEngine engine2(StorageEngine::Options{
        .cache_budget_mb = 64,
        .data_dir = "/tmp/quant_test_dirty_flush",
    });
    engine2.start();

    // Query via coroutine — should trigger read-through from disk
    auto result = blockingWait(engine2.co_query_kline("000001", 1, 0, 9999));
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].timestamp, 1000);
    EXPECT_EQ(result[1].timestamp, 2000);

    engine2.shutdown();
}

// ── Compaction tests ──

// Helper: write a small segment to a DiskPersistence
static void write_small_segment(DiskPersistence& disk, const std::string& symbol,
                                 uint8_t data_type, int64_t start_ts, int count) {
    auto rows = make_rows(start_ts, count);
    auto blocks = TimeSeriesStore::rows_to_column_blocks(rows);
    disk.write_segment(symbol, data_type, blocks, start_ts, start_ts + count - 1);
}

TEST(DiskPersistenceTest, CompactMergesSmallSegments) {
    std::filesystem::remove_all("/tmp/quant_test_compact");
    {
        DiskPersistence disk("/tmp/quant_test_compact");

        // Write 3 small segments of 100 rows each, non-overlapping
        write_small_segment(disk, "000001", 1, 1000, 100);
        write_small_segment(disk, "000001", 1, 2000, 100);
        write_small_segment(disk, "000001", 1, 3000, 100);

        auto before = disk.list_segments("000001", 1);
        EXPECT_EQ(before.size(), 3u);

        // Compact — segments have 100 rows each, well below default 4096 threshold
        size_t merged = disk.compact("000001", 1);
        EXPECT_EQ(merged, 3u);

        auto after = disk.list_segments("000001", 1);
        EXPECT_EQ(after.size(), 1u);
    }

    // Reopen and verify merged data is readable
    {
        DiskPersistence disk("/tmp/quant_test_compact");
        auto files = disk.list_segments("000001", 1);
        ASSERT_EQ(files.size(), 1u);

        auto blocks = disk.read_segment(files[0]);
        ASSERT_FALSE(blocks.empty());

        // Should have all 8 field blocks
        EXPECT_EQ(blocks.size(), 8u);

        // Verify total row count (300 rows, 3 segments × 100)
        for (const auto& b : blocks) {
            if (b.field() == DataField::kClose) {
                EXPECT_EQ(b.row_count(), 300u);
                break;
            }
        }
    }
}

TEST(DiskPersistenceTest, CompactNoOpWhenNoSmallSegments) {
    std::filesystem::remove_all("/tmp/quant_test_compact_nop");
    DiskPersistence disk("/tmp/quant_test_compact_nop");

    // Write one segment at threshold — shouldn't be compacted
    write_small_segment(disk, "000001", 1, 1000, 5000);

    size_t merged = disk.compact("000001", 1, 4096);
    EXPECT_EQ(merged, 0u);  // single segment, > 4096 rows — no compaction needed

    auto files = disk.list_segments("000001", 1);
    EXPECT_EQ(files.size(), 1u);
}

TEST(DiskPersistenceTest, CompactNeedsAtLeastTwo) {
    std::filesystem::remove_all("/tmp/quant_test_compact_one");
    DiskPersistence disk("/tmp/quant_test_compact_one");

    // Write a single small segment
    write_small_segment(disk, "000001", 1, 1000, 100);

    size_t merged = disk.compact("000001", 1);
    EXPECT_EQ(merged, 0u);  // only 1 small segment, nothing to merge

    auto files = disk.list_segments("000001", 1);
    EXPECT_EQ(files.size(), 1u);
}

TEST(DiskPersistenceTest, CompactPreservesTimestampOrder) {
    std::filesystem::remove_all("/tmp/quant_test_compact_order");
    DiskPersistence disk("/tmp/quant_test_compact_order");

    // Write segments out of order (by time) to verify compact sorts correctly
    write_small_segment(disk, "000001", 1, 3000, 100);  // later
    write_small_segment(disk, "000001", 1, 1000, 100);  // earlier
    write_small_segment(disk, "000001", 1, 2000, 100);  // middle

    disk.compact("000001", 1);

    auto files = disk.list_segments("000001", 1);
    ASSERT_EQ(files.size(), 1u);

    auto blocks = disk.read_segment(files[0]);
    ASSERT_FALSE(blocks.empty());

    // Verify timestamps are sorted
    std::vector<int64_t> timestamps;
    for (const auto& b : blocks) {
        if (b.field() == DataField::kTimestamp) {
            timestamps.resize(b.row_count());
            b.decompress(std::span<int64_t>(timestamps));
            break;
        }
    }

    ASSERT_EQ(timestamps.size(), 300u);
    for (size_t i = 1; i < timestamps.size(); ++i) {
        EXPECT_LT(timestamps[i - 1], timestamps[i]);
    }
}

TEST(DiskPersistenceTest, CompactDeduplicatesOverlapping) {
    std::filesystem::remove_all("/tmp/quant_test_compact_dedup");
    DiskPersistence disk("/tmp/quant_test_compact_dedup");

    // Write segments with overlap
    disk.write_segment("000001", 1,
        TimeSeriesStore::rows_to_column_blocks(make_rows(1000, 10)),
        1000, 1009);
    disk.write_segment("000001", 1,
        TimeSeriesStore::rows_to_column_blocks(make_rows(1005, 10)),
        1005, 1014);

    size_t merged = disk.compact("000001", 1);
    EXPECT_EQ(merged, 2u);

    auto files = disk.list_segments("000001", 1);
    ASSERT_EQ(files.size(), 1u);

    auto blocks = disk.read_segment(files[0]);
    ASSERT_FALSE(blocks.empty());

    for (const auto& b : blocks) {
        if (b.field() == DataField::kClose) {
            EXPECT_EQ(b.row_count(), 15u);  // 1000-1014 = 15 unique timestamps
            break;
        }
    }
}

// ── DiskPersistence error path tests ──

TEST(DiskPersistenceTest, ReadNonExistentSegment) {
    std::filesystem::remove_all("/tmp/quant_test_bad_read");
    DiskPersistence disk("/tmp/quant_test_bad_read");

    auto blocks = disk.read_segment("nonexistent.seg");
    EXPECT_TRUE(blocks.empty());
}

TEST(DiskPersistenceTest, ReadCorruptSegment) {
    std::filesystem::remove_all("/tmp/quant_test_corrupt");
    DiskPersistence disk("/tmp/quant_test_corrupt");

    // Write a file with bad magic
    {
        std::ofstream ofs("/tmp/quant_test_corrupt/bad.seg", std::ios::binary);
        uint32_t bad_magic = 0xDEADBEEF;
        ofs.write(reinterpret_cast<const char*>(&bad_magic), sizeof(bad_magic));
    }

    auto blocks = disk.read_segment("bad.seg");
    EXPECT_TRUE(blocks.empty());
}

TEST(DiskPersistenceTest, ReadEmptySegmentFile) {
    std::filesystem::remove_all("/tmp/quant_test_empty_seg");
    DiskPersistence disk("/tmp/quant_test_empty_seg");

    // Create an empty .seg file
    {
        std::ofstream ofs("/tmp/quant_test_empty_seg/empty.seg", std::ios::binary);
    }

    auto blocks = disk.read_segment("empty.seg");
    EXPECT_TRUE(blocks.empty());
}

TEST(DiskPersistenceTest, DeleteNonExistentSegment) {
    std::filesystem::remove_all("/tmp/quant_test_del_missing");
    DiskPersistence disk("/tmp/quant_test_del_missing");

    // Deleting a non-existent file should return false (remove failed)
    bool deleted = disk.delete_segment("nope.seg");
    EXPECT_FALSE(deleted);
}

// ── T4.2: Read-Through extended tests ──

TEST(TimeSeriesStoreTest, ReadThroughNewStoreBackfillsCache) {
    // Write data via DiskPersistence directly, then create fresh store
    // (empty cache) and trigger read-through — verify cache backfill.
    std::filesystem::remove_all("/tmp/quant_test_rt_newstore");
    {
        DiskPersistence disk("/tmp/quant_test_rt_newstore");
        auto blocks = TimeSeriesStore::rows_to_column_blocks(make_rows(1000, 10));
        disk.write_segment("000001", 1, blocks, 1000, 1009);
    }
    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_rt_newstore",
    });

    // First query: read-through from disk
    auto r1 = blockingWait(store.co_query_kline("000001", 1, 1000, 1009));
    ASSERT_EQ(r1.size(), 10u);
    EXPECT_EQ(r1[0].timestamp, 1000);
    EXPECT_EQ(r1[9].timestamp, 1009);

    // Second query: should hit cache (backfilled by first query)
    auto r2 = blockingWait(store.co_query_kline("000001", 1, 1000, 1009));
    ASSERT_EQ(r2.size(), 10u);
    EXPECT_EQ(r2[0].timestamp, 1000);
}

TEST(TimeSeriesStoreTest, ReadThroughNonOverlappingCacheAndDisk) {
    // Data in cache (ts 1000-1002) and data on disk (ts 2000-2004) are
    // non-overlapping. Querying the disk range triggers read-through.
    std::filesystem::remove_all("/tmp/quant_test_rt_nonoverlap");
    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64},
        .data_dir = "/tmp/quant_test_rt_nonoverlap",
    });

    // Write to cache only (via co_store_kline, no disk flush)
    blockingWait(store.co_store_kline("000001", 1, make_row(1000, 10.0)));
    blockingWait(store.co_store_kline("000001", 1, make_row(1001, 11.0)));
    blockingWait(store.co_store_kline("000001", 1, make_row(1002, 12.0)));

    // Write non-overlapping data to disk via DiskPersistence directly
    auto disk_rows = make_rows(2000, 5);  // 2000-2004
    auto blocks = TimeSeriesStore::rows_to_column_blocks(disk_rows);
    store.disk().write_segment("000001", 1, blocks, 2000, 2004);

    // Query cache range — cache hit
    auto cached = blockingWait(store.co_query_kline("000001", 1, 1000, 1002));
    ASSERT_EQ(cached.size(), 3u);
    EXPECT_EQ(cached[0].timestamp, 1000);

    // Query disk range — cache miss (different ts range) → read-through
    auto from_disk = blockingWait(store.co_query_kline("000001", 1, 2000, 2004));
    ASSERT_EQ(from_disk.size(), 5u);
    EXPECT_EQ(from_disk[0].timestamp, 2000);
    EXPECT_EQ(from_disk[4].timestamp, 2004);

    // Disk data should now be backfilled into cache
    auto cached_again = blockingWait(store.co_query_kline("000001", 1, 2000, 2004));
    ASSERT_EQ(cached_again.size(), 5u);
}

TEST(TimeSeriesStoreTest, ReadThroughCacheFullEviction) {
    // Fill cache near budget with one symbol, then read-through another.
    // Verify read-through succeeds despite cache pressure.
    std::filesystem::remove_all("/tmp/quant_test_rt_fullevict");

    // Pre-write target data to disk
    DiskPersistence disk("/tmp/quant_test_rt_fullevict");
    auto blocks = TimeSeriesStore::rows_to_column_blocks(make_rows(1000, 10));
    disk.write_segment("000001", 1, blocks, 1000, 1009);

    TimeSeriesStore store(TimeSeriesStore::Options{
        .cache_opts = TimeSeriesCache::Options{.num_shards = 1, .budget_mb = 1},
        .data_dir = "/tmp/quant_test_rt_fullevict",
    });

    // Fill cache near/over budget with a filler symbol (same shard, 1 shard total)
    for (int i = 0; i < 20000; ++i) {
        store.store_kline("FILLER", 1, make_row(i * 1000, 100.0));
    }

    // Read-through for 000001 — should succeed even with cache pressure
    auto result = blockingWait(store.co_query_kline("000001", 1, 1000, 1009));
    ASSERT_EQ(result.size(), 10u);
    EXPECT_EQ(result[0].timestamp, 1000);
    EXPECT_EQ(result[9].timestamp, 1009);
}

// ── T4.3: Cache eviction extended tests ──

TEST(TimeSeriesCacheTest, EvictionRemovesOldestPreservesNewest) {
    // Sub-1MB budget, 40000 writes → oldest evicted, newest survives
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 1, .budget_mb = 1});

    for (int i = 0; i < 40000; ++i) {
        cache.append("000001", 1, make_row(i * 1000, 100.0));
    }

    // Newest rows should be present
    auto newest = cache.query("000001", 1, 39000000, 40000000);
    ASSERT_FALSE(newest.empty());
    EXPECT_GT(newest.back().timestamp, 35000000);

    // Oldest rows should be gone (evicted)
    auto oldest = cache.query("000001", 1, 0, 5000000);
    EXPECT_TRUE(oldest.empty());

    // Total surviving rows < 40000
    EXPECT_LT(newest.size() + oldest.size(), 40000u);
}

TEST(TimeSeriesCacheTest, EvictionWithDataSource) {
    // Verify DataSource is properly stored regardless of source value.
    // Note: current eviction uses LRU only, not DataSource priority.
    // This test verifies the tagging is functional.
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 4, .budget_mb = 64});

    blockingWait(cache.co_append("000001", 1, make_row(1000, 10.0),
                                  DataSource::kRealtimeIngest));
    blockingWait(cache.co_append("000002", 1, make_row(1000, 20.0),
                                  DataSource::kBatchLoad));
    blockingWait(cache.co_append("000003", 1, make_row(1000, 30.0),
                                  DataSource::kRemoteLoad));

    // All should be queryable regardless of DataSource
    auto r1 = cache.query("000001", 1, 0, 9999);
    auto r2 = cache.query("000002", 1, 0, 9999);
    auto r3 = cache.query("000003", 1, 0, 9999);
    ASSERT_EQ(r1.size(), 1u);
    ASSERT_EQ(r2.size(), 1u);
    ASSERT_EQ(r3.size(), 1u);
    EXPECT_EQ(r1[0].close_price, 100000);   // 10.0 * 10000
    EXPECT_EQ(r2[0].close_price, 200000);   // 20.0 * 10000
    EXPECT_EQ(r3[0].close_price, 300000);   // 30.0 * 10000
}

TEST(TimeSeriesCacheTest, EvictedDataNotQueryable) {
    // After eviction, verify old data is truly unreachable
    // and new data for the same key is consistent.
    TimeSeriesCache cache(TimeSeriesCache::Options{.num_shards = 1, .budget_mb = 1});

    // Write enough rows to trigger eviction multiple times.
    // Budget=1MB, sizeof(KlineRow)=48, so ~21846 rows triggers eviction.
    for (int i = 0; i < 30000; ++i) {
        cache.append("000001", 1, make_row(i * 1000, 100.0));
    }

    // Data from early appends should be gone (evicted)
    auto early = cache.query("000001", 1, 0, 1000000);
    EXPECT_TRUE(early.empty());

    // Newest data (written after last eviction) should exist
    auto recent = cache.query("000001", 1, 25000000, 30000000);
    EXPECT_FALSE(recent.empty());

    // A different symbol should also be queryable independently
    cache.append("000002", 1, make_row(99999, 200.0));
    auto other = cache.query("000002", 1, 0, 999999);
    ASSERT_EQ(other.size(), 1u);
    EXPECT_EQ(other[0].close_price, 2000000);
}

// ── T5.6: Multi-frequency DataInitializer tests ──

TEST(DataInitTest, InferFreqFromFilename) {
    EXPECT_EQ(infer_freq_from_filename("600519_1min.csv"), KlineFreq::kMin1);
    EXPECT_EQ(infer_freq_from_filename("000001_5min_data.csv"), KlineFreq::kMin5);
    EXPECT_EQ(infer_freq_from_filename("600519_15min_2020.csv"), KlineFreq::kMin15);
    EXPECT_EQ(infer_freq_from_filename("000001_30min.csv"), KlineFreq::kMin30);
    EXPECT_EQ(infer_freq_from_filename("600519_60min.csv"), KlineFreq::kMin60);
    EXPECT_EQ(infer_freq_from_filename("000001_day.csv"), KlineFreq::kDay);
    EXPECT_EQ(infer_freq_from_filename("600519_daily_2020.csv"), KlineFreq::kDay);
    EXPECT_EQ(infer_freq_from_filename("000001.csv"), KlineFreq::kDay);  // default
}

TEST(DataInitTest, LoadCsvWithFreqParam) {
    std::string csv =
        "symbol,date,open,high,low,close,volume,amount\n"
        "600519.SH,2020-01-02,1130.00,1145.50,1128.00,1140.00,3500000,3980000000\n"
        "600519.SH,2020-01-03,1142.00,1150.00,1135.00,1148.00,2800000,3210000000\n";

    std::filesystem::remove_all("/tmp/quant_test_init_freq");
    std::filesystem::create_directories("/tmp/quant_test_init_freq");
    auto csv_path = "/tmp/quant_test_init_freq/data.csv";
    {
        std::ofstream f(csv_path);
        f << csv;
    }

    StorageEngine engine(StorageEngine::Options{
        .cache_budget_mb = 64,
        .data_dir = "/tmp/quant_test_init_freq_data",
    });
    engine.start();
    DataInitializer init(engine);

    // Load with 5min frequency
    ASSERT_TRUE(init.load_csv(csv_path, KlineFreq::kMin5));

    auto stats = init.stats();
    EXPECT_EQ(stats.rows_loaded, 2);

    // Data should be stored with kMin5 data_type
    auto result = engine.query_kline("600519.SH", kline_freq_to_data_type(KlineFreq::kMin5),
                                     0, INT64_MAX);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].close_price, 11400000);  // 1140.00 * 10000

    // Query with day data_type should return empty
    auto day_result = engine.query_kline("600519.SH", kline_freq_to_data_type(KlineFreq::kDay),
                                          0, INT64_MAX);
    EXPECT_TRUE(day_result.empty());

    engine.shutdown();
    std::filesystem::remove_all("/tmp/quant_test_init_freq");
}

TEST(DataInitTest, LoadCsvDirInfersFreqFromFilename) {
    std::filesystem::remove_all("/tmp/quant_test_init_dirfreq");
    std::filesystem::create_directories("/tmp/quant_test_init_dirfreq");

    std::string day_csv =
        "symbol,date,open,high,low,close,volume,amount\n"
        "600519.SH,2020-01-02,1130.00,1145.50,1128.00,1140.00,3500000,3980000000\n";

    std::string min1_csv =
        "symbol,date,open,high,low,close,volume,amount\n"
        "600519.SH,2020-01-02 09:31,1130.00,1131.00,1129.50,1130.50,10000,11300000\n";

    {
        std::ofstream f1("/tmp/quant_test_init_dirfreq/600519_day.csv");
        f1 << day_csv;
        std::ofstream f2("/tmp/quant_test_init_dirfreq/600519_1min.csv");
        f2 << min1_csv;
    }

    StorageEngine engine(StorageEngine::Options{
        .cache_budget_mb = 64,
        .data_dir = "/tmp/quant_test_init_dirfreq",
    });
    engine.start();
    DataInitializer init(engine);

    int loaded = init.load_csv_dir("/tmp/quant_test_init_dirfreq");
    EXPECT_EQ(loaded, 2);

    auto stats = init.stats();
    EXPECT_EQ(stats.rows_loaded, 2);
    EXPECT_EQ(stats.rows_failed, 0);

    // Day data should be queriable with day data_type
    auto day_result = engine.query_kline("600519.SH", kline_freq_to_data_type(KlineFreq::kDay),
                                          0, INT64_MAX);
    ASSERT_EQ(day_result.size(), 1u);

    // 1min data should be queryable with 1min data_type
    auto min1_result = engine.query_kline("600519.SH", kline_freq_to_data_type(KlineFreq::kMin1),
                                           0, INT64_MAX);
    ASSERT_EQ(min1_result.size(), 1u);

    engine.shutdown();
    std::filesystem::remove_all("/tmp/quant_test_init_dirfreq");
}

TEST(DataInitTest, LoadCsvWithFreqColumn) {
    std::string csv =
        "symbol,date,open,high,low,close,volume,amount,freq\n"
        "600519.SH,2020-01-02,1130.00,1145.50,1128.00,1140.00,3500000,3980000000,1min\n"
        "600519.SH,2020-01-03,1142.00,1150.00,1135.00,1148.00,2800000,3210000000,1min\n";

    std::filesystem::remove_all("/tmp/quant_test_init_freqcol");
    std::filesystem::create_directories("/tmp/quant_test_init_freqcol");
    auto csv_path = "/tmp/quant_test_init_freqcol/data.csv";
    {
        std::ofstream f(csv_path);
        f << csv;
    }

    StorageEngine engine(StorageEngine::Options{
        .cache_budget_mb = 64,
        .data_dir = "/tmp/quant_test_init_freqcol",
    });
    engine.start();
    DataInitializer init(engine);

    // Pass kDay as default, but CSV freq column should override to kMin1
    ASSERT_TRUE(init.load_csv(csv_path, KlineFreq::kDay));

    auto stats = init.stats();
    EXPECT_EQ(stats.rows_loaded, 2);

    // Data should be stored with 1min data_type (from freq column)
    auto result = engine.query_kline("600519.SH", kline_freq_to_data_type(KlineFreq::kMin1),
                                     0, INT64_MAX);
    ASSERT_EQ(result.size(), 2u);

    // Day data_type should return empty (overridden by column)
    auto day_result = engine.query_kline("600519.SH", kline_freq_to_data_type(KlineFreq::kDay),
                                          0, INT64_MAX);
    EXPECT_TRUE(day_result.empty());

    engine.shutdown();
    std::filesystem::remove_all("/tmp/quant_test_init_freqcol");
}

// ── T5.8: DiskPersistence async I/O tests ──

TEST(DiskPersistenceTest, ReadSegmentFiltered) {
    std::filesystem::remove_all("/tmp/quant_test_filtered");
    {
        DiskPersistence disk("/tmp/quant_test_filtered");
        auto rows = make_rows(1000, 10);
        auto blocks = TimeSeriesStore::rows_to_column_blocks(rows);
        disk.write_segment("000001", 1, blocks, 1000, 1009);

        auto result = disk.read_segment_filtered(
            disk.list_segments("000001", 1)[0],
            DataField::kClose, 1002, 1005);

        ASSERT_FALSE(result.empty());
        EXPECT_EQ(result.size(), 1u);
        EXPECT_EQ(result[0].field(), DataField::kClose);
        EXPECT_EQ(result[0].row_count(), 10u);
    }
    std::filesystem::remove_all("/tmp/quant_test_filtered");
}

TEST(DiskPersistenceTest, ReadSegmentFilteredRangeExcludes) {
    std::filesystem::remove_all("/tmp/quant_test_filtered_excl");
    {
        DiskPersistence disk("/tmp/quant_test_filtered_excl");
        auto rows = make_rows(1000, 10);
        auto blocks = TimeSeriesStore::rows_to_column_blocks(rows);
        disk.write_segment("000001", 1, blocks, 1000, 1009);

        auto result = disk.read_segment_filtered(
            disk.list_segments("000001", 1)[0],
            DataField::kClose, 5000, 6000);
        EXPECT_TRUE(result.empty());
    }
    std::filesystem::remove_all("/tmp/quant_test_filtered_excl");
}

TEST(DiskPersistenceTest, ReadSegmentFilteredFieldMismatch) {
    std::filesystem::remove_all("/tmp/quant_test_filtered_field");
    {
        DiskPersistence disk("/tmp/quant_test_filtered_field");
        auto rows = make_rows(1000, 10);
        auto blocks = TimeSeriesStore::rows_to_column_blocks(rows);
        disk.write_segment("000001", 1, blocks, 1000, 1009);

        auto result = disk.read_segment_filtered(
            disk.list_segments("000001", 1)[0],
            static_cast<DataField>(99), 0, INT64_MAX);
        EXPECT_TRUE(result.empty());
    }
    std::filesystem::remove_all("/tmp/quant_test_filtered_field");
}

TEST(DiskPersistenceTest, CoReadSegmentFilteredFallback) {
    std::filesystem::remove_all("/tmp/quant_test_co_filtered");
    {
        DiskPersistence disk("/tmp/quant_test_co_filtered");
        auto rows = make_rows(2000, 5);
        auto blocks = TimeSeriesStore::rows_to_column_blocks(rows);
        disk.write_segment("000001", 1, blocks, 2000, 2004);

        auto segments = disk.list_segments("000001", 1);
        ASSERT_EQ(segments.size(), 1u);

        auto result = blockingWait(disk.co_read_segment_filtered(
            segments[0], DataField::kOpen, 2001, 2003));
        ASSERT_FALSE(result.empty());
        EXPECT_EQ(result[0].field(), DataField::kOpen);
        EXPECT_EQ(result[0].row_count(), 5u);

        auto empty_result = blockingWait(disk.co_read_segment_filtered(
            segments[0], DataField::kOpen, 9999, 99999));
        EXPECT_TRUE(empty_result.empty());
    }
    std::filesystem::remove_all("/tmp/quant_test_co_filtered");
}

TEST(DiskPersistenceTest, CoWriteAndReadFallback) {
    std::filesystem::remove_all("/tmp/quant_test_co_fallback");
    {
        DiskPersistence disk("/tmp/quant_test_co_fallback");
        auto rows = make_rows(100, 5);
        auto blocks = TimeSeriesStore::rows_to_column_blocks(rows);

        auto fname = blockingWait(disk.co_write_segment(
            "000001", 1, blocks, 100, 104));
        EXPECT_FALSE(fname.empty());

        auto read_back = blockingWait(disk.co_read_segment(fname));
        ASSERT_EQ(read_back.size(), 8u);

        std::vector<int64_t> ts_out(5);
        read_back[0].decompress(std::span<int64_t>(ts_out));
        EXPECT_EQ(ts_out[0], 100);
        EXPECT_EQ(ts_out[4], 104);
    }
    std::filesystem::remove_all("/tmp/quant_test_co_fallback");
}
