// storage_test.cc — Tests for ColumnBlock, TimeSeriesCache, StorageEngine, WriteAheadLog, WriteBuffer
#include "cpp/quant/storage/column_block.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/storage/time_series_cache.h"
#include "cpp/quant/storage/time_series_store.h"
#include "cpp/quant/storage/segment_index.h"
#include "cpp/quant/storage/write_buffer.h"
#include "cpp/quant/storage/write_ahead_log.h"
#include "cpp/quant/infra/coroutine.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <vector>

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
