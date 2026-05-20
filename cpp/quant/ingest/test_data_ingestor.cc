// test_data_ingestor.cc — Unit tests for DataIngestor
//
// Tests:
// 1. Manual kline ingestion (no network)
// 2. Statistics tracking
// 3. Stop/shutdown

#include <gtest/gtest.h>
#include <filesystem>

#include "cpp/quant/ingest/data_ingestor.h"
#include "cpp/quant/storage/time_series_store.h"
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/kline_event.h"

using namespace quant::ingest;
using namespace quant::storage;
using namespace quant::event;

class DataIngestorTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "ingestor_test";
        std::filesystem::create_directories(test_dir_);

        store_ = std::make_unique<TimeSeriesStore>(64, test_dir_.string());
        bus_ = std::make_unique<EventBus>(EventBus::Options{});
    }

    void TearDown() override {
        store_.reset();
        bus_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    DataSourceConfig make_config() {
        DataSourceConfig cfg;
        cfg.name = "test_source";
        cfg.host = "127.0.0.1";
        cfg.port = 9999;
        cfg.symbols = {"000001.SH", "600519.SH"};
        return cfg;
    }

    KlineData make_kline(int64_t ts, int64_t close) {
        return KlineData{
            .timestamp = ts,
            .open = close - 100,
            .high = close + 200,
            .low = close - 200,
            .close = close,
            .volume = 1000000,
            .amount = close * 1000000,
        };
    }

    std::filesystem::path test_dir_;
    std::unique_ptr<TimeSeriesStore> store_;
    std::unique_ptr<EventBus> bus_;
};

// ── Test: Manual kline ingestion ──
TEST_F(DataIngestorTest, ManualIngestKline) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    auto kline = make_kline(1700000000000, 34000000);
    ASSERT_TRUE(ingestor.ingest_kline("000001.SH", kline));

    auto stats = ingestor.stats();
    EXPECT_EQ(stats.klines_received, 1);
    EXPECT_EQ(stats.klines_stored, 1);
    EXPECT_EQ(stats.klines_failed, 0);
}

// ── Test: Multiple kline ingestion ──
TEST_F(DataIngestorTest, MultipleKlineIngestion) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    for (int i = 0; i < 100; i++) {
        auto kline = make_kline(1700000000000 + i * 60000, 34000000 + i * 1000);
        ASSERT_TRUE(ingestor.ingest_kline("000001.SH", kline));
    }

    auto stats = ingestor.stats();
    EXPECT_EQ(stats.klines_received, 100);
    EXPECT_EQ(stats.klines_stored, 100);
}

// ── Test: Statistics tracking ──
TEST_F(DataIngestorTest, StatisticsTracking) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    auto stats = ingestor.stats();
    EXPECT_EQ(stats.klines_received, 0);
    EXPECT_EQ(stats.klines_stored, 0);
    EXPECT_FALSE(stats.connected);

    for (int i = 0; i < 10; i++) {
        ingestor.ingest_kline("000001.SH", make_kline(i, 34000000 + i * 1000));
    }

    stats = ingestor.stats();
    EXPECT_EQ(stats.klines_received, 10);
    EXPECT_EQ(stats.klines_stored, 10);
}

// ── Test: Stop/shutdown ──
TEST_F(DataIngestorTest, StopShutdown) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    EXPECT_FALSE(ingestor.is_running());

    ingestor.stop();
    EXPECT_FALSE(ingestor.is_running());
}

// ── Test: Event publishing on ingest ──
TEST_F(DataIngestorTest, EventPublishing) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    // Subscribe to kline events using IEventSubscriber
    struct KlineCounter : public IEventSubscriber {
        int count = 0;
        void on_event(const Event& event) override {
            if (event.event_type_id() == KlineEvent::kEventTypeId) {
                count++;
            }
        }
    };

    auto counter = std::make_unique<KlineCounter>();
    auto* counter_ptr = counter.get();
    bus_->subscribe(KlineEvent::kEventTypeId, std::move(counter));

    ingestor.ingest_kline("000001.SH", make_kline(1700000000000, 34000000));
    ingestor.ingest_kline("600519.SH", make_kline(1700000000000, 180000000));

    EXPECT_EQ(counter_ptr->count, 2);
}

// ── Test: Multiple symbols ──
TEST_F(DataIngestorTest, MultipleSymbols) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    ingestor.ingest_kline("000001.SH", make_kline(1700000000000, 34000000));
    ingestor.ingest_kline("600519.SH", make_kline(1700000000000, 180000000));
    ingestor.ingest_kline("000002.SZ", make_kline(1700000000000, 15000000));

    auto stats = ingestor.stats();
    EXPECT_EQ(stats.klines_received, 3);
    EXPECT_EQ(stats.klines_stored, 3);
}
