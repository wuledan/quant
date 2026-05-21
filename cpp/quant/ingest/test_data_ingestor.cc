// test_data_ingestor.cc — Unit tests for DataIngestor
//
// Tests:
// 1. Manual kline ingestion (no network)
// 2. Statistics tracking
// 3. Stop/shutdown
// 4. JSON parsing — float prices (×10000 conversion)
// 5. JSON parsing — integer prices (backward compat)
// 6. JSON parsing — error cases
// 7. Multiple symbols
// 8. Event publishing

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

// ── Test: Parse kline with float prices (×10000 conversion) ──
// This tests the network JSON format where prices arrive as floats.
TEST_F(DataIngestorTest, ParseKlineFloatPrices) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    // JSON with float prices — typical network format
    const char* json = R"({"symbol":"600519.SH","ts":1700000000000,"open":3400.50,"high":3420.00,"low":3380.25,"close":3410.75,"volume":1500000,"amount":5125000000})";
    size_t len = std::strlen(json);

    std::string symbol;
    KlineData kline{};
    bool ok = ingestor.parse_kline(json, len, symbol, kline);

    ASSERT_TRUE(ok);
    EXPECT_EQ(symbol, "600519.SH");
    EXPECT_EQ(kline.timestamp, 1700000000000);

    // 3400.50 * 10000 = 34005000
    EXPECT_EQ(kline.open, 34005000);
    // 3420.00 * 10000 = 34200000
    EXPECT_EQ(kline.high, 34200000);
    // 3380.25 * 10000 = 33802500
    EXPECT_EQ(kline.low, 33802500);
    // 3410.75 * 10000 = 34107500
    EXPECT_EQ(kline.close, 34107500);

    EXPECT_EQ(kline.volume, 1500000);
    EXPECT_EQ(kline.amount, 5125000000);
}

// ── Test: Parse kline with integer prices (backward compat) ──
// When prices are already in fixed-point (e.g. from internal sources).
TEST_F(DataIngestorTest, ParseKlineIntegerPrices) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    // JSON with integer prices — already in fixed-point
    const char* json = R"({"symbol":"000001.SH","ts":1700000000000,"open":34000000,"high":34200000,"low":33800000,"close":34100000,"volume":1000000,"amount":34000000000})";
    size_t len = std::strlen(json);

    std::string symbol;
    KlineData kline{};
    bool ok = ingestor.parse_kline(json, len, symbol, kline);

    ASSERT_TRUE(ok);
    EXPECT_EQ(symbol, "000001.SH");
    EXPECT_EQ(kline.open, 34000000);
    EXPECT_EQ(kline.high, 34200000);
    EXPECT_EQ(kline.low, 33800000);
    EXPECT_EQ(kline.close, 34100000);
}

// ── Test: Parse kline error cases ──
TEST_F(DataIngestorTest, ParseKlineErrors) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    std::string symbol;
    KlineData kline{};

    // Empty input
    EXPECT_FALSE(ingestor.parse_kline("", 0, symbol, kline));

    // Too short
    EXPECT_FALSE(ingestor.parse_kline("{}", 2, symbol, kline));

    // Missing symbol
    const char* json_no_symbol = R"({"ts":1700000000000,"open":3400.50,"high":3420.00,"low":3380.25,"close":3410.75,"volume":1500000})";
    EXPECT_FALSE(ingestor.parse_kline(json_no_symbol, std::strlen(json_no_symbol),
                                       symbol, kline));

    // Missing timestamp
    const char* json_no_ts = R"({"symbol":"600519.SH","open":3400.50,"high":3420.00,"low":3380.25,"close":3410.75,"volume":1500000})";
    EXPECT_FALSE(ingestor.parse_kline(json_no_ts, std::strlen(json_no_ts),
                                       symbol, kline));

    // Missing close price
    const char* json_no_close = R"({"symbol":"600519.SH","ts":1700000000000,"open":3400.50,"high":3420.00,"low":3380.25,"volume":1500000})";
    EXPECT_FALSE(ingestor.parse_kline(json_no_close, std::strlen(json_no_close),
                                       symbol, kline));
}

// ── Test: Parse kline with mixed float/int fields ──
TEST_F(DataIngestorTest, ParseKlineMixedPrices) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    // Some prices as float, some as int
    const char* json = R"({"symbol":"000001.SZ","ts":1700000000000,"open":15.30,"high":156000,"low":151000,"close":15.55,"volume":800000,"amount":12400000})";
    size_t len = std::strlen(json);

    std::string symbol;
    KlineData kline{};
    bool ok = ingestor.parse_kline(json, len, symbol, kline);

    ASSERT_TRUE(ok);
    EXPECT_EQ(symbol, "000001.SZ");
    // 15.30 * 10000 = 153000
    EXPECT_EQ(kline.open, 153000);
    // 156000 (integer, no decimal point → extract_double fails → extract_int64)
    EXPECT_EQ(kline.high, 156000);
    // 151000 (integer)
    EXPECT_EQ(kline.low, 151000);
    // 15.55 * 10000 = 155500
    EXPECT_EQ(kline.close, 155500);
}

// ── Test: Ingest kline from parsed JSON (float prices) ──
TEST_F(DataIngestorTest, IngestFromParsedFloatJson) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    const char* json = R"({"symbol":"600519.SH","ts":1700000000000,"open":3400.50,"high":3420.00,"low":3380.25,"close":3410.75,"volume":1500000,"amount":5125000000})";
    size_t len = std::strlen(json);

    std::string symbol;
    KlineData kline{};
    ASSERT_TRUE(ingestor.parse_kline(json, len, symbol, kline));

    // Ingest the parsed kline
    EXPECT_TRUE(ingestor.ingest_kline(symbol, kline));

    auto stats = ingestor.stats();
    EXPECT_EQ(stats.klines_received, 1);
    EXPECT_EQ(stats.klines_stored, 1);
    EXPECT_EQ(stats.klines_failed, 0);
}

// ── Test: Event content verification ──
TEST_F(DataIngestorTest, EventContentVerification) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    struct KlineCapture : public IEventSubscriber {
        std::string last_symbol;
        KlineRow last_kline{};
        int count = 0;
        void on_event(const Event& event) override {
            if (event.event_type_id() == KlineEvent::kEventTypeId) {
                const auto& ke = static_cast<const KlineEvent&>(event);
                last_symbol = ke.symbol;
                last_kline = ke.kline;
                count++;
            }
        }
    };

    auto capture = std::make_unique<KlineCapture>();
    auto* cap = capture.get();
    bus_->subscribe(KlineEvent::kEventTypeId, std::move(capture));

    // Ingest a kline with known values
    KlineData kline{
        .timestamp = 1700000000000,
        .open = 34005000,    // 3400.50 * 10000
        .high = 34200000,    // 3420.00 * 10000
        .low = 33802500,     // 3380.25 * 10000
        .close = 34107500,   // 3410.75 * 10000
        .volume = 1500000,
        .amount = 5125000000,
    };
    ingestor.ingest_kline("600519.SH", kline);

    ASSERT_EQ(cap->count, 1);
    EXPECT_EQ(cap->last_symbol, "600519.SH");
    EXPECT_EQ(cap->last_kline.timestamp, 1700000000000);
    EXPECT_EQ(cap->last_kline.open_price, 34005000);
    EXPECT_EQ(cap->last_kline.high_price, 34200000);
    EXPECT_EQ(cap->last_kline.low_price, 33802500);
    EXPECT_EQ(cap->last_kline.close_price, 34107500);
    EXPECT_EQ(cap->last_kline.volume, 1500000);
    EXPECT_EQ(cap->last_kline.amount, 5125000000);
}

// ── Test: Parse kline with whitespace in JSON ──
TEST_F(DataIngestorTest, ParseKlineWithWhitespace) {
    DataIngestor ingestor(*store_, *bus_, make_config());

    const char* json = R"({ "symbol" : "000001.SH" , "ts" : 1700000000000 , "open" : 3400.50 , "high" : 3420.00 , "low" : 3380.25 , "close" : 3410.75 , "volume" : 1500000 , "amount" : 5125000000 })";
    size_t len = std::strlen(json);

    std::string symbol;
    KlineData kline{};
    bool ok = ingestor.parse_kline(json, len, symbol, kline);

    ASSERT_TRUE(ok);
    EXPECT_EQ(symbol, "000001.SH");
    EXPECT_EQ(kline.open, 34005000);
    EXPECT_EQ(kline.close, 34107500);
}
