// tushare_ingestor_test.cc — Unit tests for TushareIngestor parsing/conversion
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "cpp/quant/ingest/tushare_ingestor.h"
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/storage/storage_engine.h"
#include "gtest/gtest.h"

namespace quant::ingest {

// Test accessor — friend of TushareIngestor, forwards private methods
class TushareIngestorTestAccessor {
public:
    static std::vector<event::KlineRow> parse_response(
        TushareIngestor& ingestor,
        const std::string& json,
        const std::string& symbol) {
        return ingestor.parse_response(json, symbol);
    }

    static int32_t price_to_fixed(TushareIngestor& ingestor, double price) {
        return ingestor.price_to_fixed(price);
    }

    static int64_t date_to_epoch_us(TushareIngestor& ingestor, const std::string& date_str) {
        return ingestor.date_to_epoch_us(date_str);
    }
};

class TushareIngestorTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = std::make_unique<storage::StorageEngine>(
            storage::StorageEngine::Options{
                .cache_budget_mb = 64,
                .data_dir = "/tmp/quant_test_tushare",
            });
        engine_->start();
        bus_ = std::make_unique<event::EventBus>(
            event::EventBus::default_options());
        ingestor_ = std::make_unique<TushareIngestor>(
            TushareIngestor::Options{
                "test_token",
                std::chrono::seconds(60),
                {"000001.SZ"}},
            *engine_, *bus_);
    }

    void TearDown() override {
        ingestor_.reset();
        bus_.reset();
        engine_->shutdown();
        engine_.reset();
    }

    std::unique_ptr<storage::StorageEngine> engine_;
    std::unique_ptr<event::EventBus> bus_;
    std::unique_ptr<TushareIngestor> ingestor_;
};

// ── price_to_fixed ──

TEST_F(TushareIngestorTest, PriceToFixedZero) {
    EXPECT_EQ(TushareIngestorTestAccessor::price_to_fixed(*ingestor_, 0.0), 0);
}

TEST_F(TushareIngestorTest, PriceToFixedInteger) {
    // 10.0 * 10000 = 100000
    EXPECT_EQ(TushareIngestorTestAccessor::price_to_fixed(*ingestor_, 10.0), 100000);
}

TEST_F(TushareIngestorTest, PriceToFixedFractional) {
    // 10.1234 * 10000 = 101234
    EXPECT_EQ(TushareIngestorTestAccessor::price_to_fixed(*ingestor_, 10.1234), 101234);
}

TEST_F(TushareIngestorTest, PriceToFixedRounding) {
    // 10.12345 → llround(101234.5) → 101235
    EXPECT_EQ(TushareIngestorTestAccessor::price_to_fixed(*ingestor_, 10.12345), 101235);

    // 10.12344 → 101234.4 → llround → 101234
    EXPECT_EQ(TushareIngestorTestAccessor::price_to_fixed(*ingestor_, 10.12344), 101234);
}

TEST_F(TushareIngestorTest, PriceToFixedNegative) {
    EXPECT_EQ(TushareIngestorTestAccessor::price_to_fixed(*ingestor_, -5.5), -55000);
}

TEST_F(TushareIngestorTest, PriceToFixedLarge) {
    // int32_t max = 2147483647, so max price ≈ 214748.3647
    // Test with 100000.00 * 10000 = 1000000000 (fits in int32_t)
    EXPECT_EQ(TushareIngestorTestAccessor::price_to_fixed(*ingestor_, 100000.00), 1000000000);
}

// ── date_to_epoch_us ──

TEST_F(TushareIngestorTest, DateToEpochUsEpoch) {
    EXPECT_EQ(TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, "19700101"), 0);
}

TEST_F(TushareIngestorTest, DateToEpochUs20260101) {
    // 2026-01-01 00:00:00 UTC = 20454 days from epoch
    // 20454 * 86400 * 1000000 = 1767225600000000
    EXPECT_EQ(TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, "20260101"),
              1767225600000000LL);
}

TEST_F(TushareIngestorTest, DateToEpochUs20260122) {
    // 2026-01-01 + 21 days = (20454 + 21) * 86400 * 1000000
    EXPECT_EQ(TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, "20260122"),
              1769040000000000LL);
}

TEST_F(TushareIngestorTest, DateToEpochUsLeapYear) {
    // 2024-02-29: 13 leap years from 1970-2023, 19723 days + 59
    // 19782 * 86400 * 1000000 = 1709164800000000
    EXPECT_EQ(TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, "20240229"),
              1709164800000000LL);
}

TEST_F(TushareIngestorTest, DateToEpochUsShortString) {
    EXPECT_EQ(TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, "202601"), 0);
    EXPECT_EQ(TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, ""), 0);
}

// ── parse_response ──

TEST_F(TushareIngestorTest, ParseResponseEmpty) {
    EXPECT_TRUE(TushareIngestorTestAccessor::parse_response(*ingestor_, "", "000001.SZ").empty());
    EXPECT_TRUE(TushareIngestorTestAccessor::parse_response(*ingestor_, "{}", "000001.SZ").empty());
}

TEST_F(TushareIngestorTest, ParseResponseSingleItem) {
    std::string json = R"({"data":{"items":[["20260122",10.5,11.2,10.3,10.8,1234567,987654321.0]]}})";
    auto rows = TushareIngestorTestAccessor::parse_response(*ingestor_, json, "000001.SZ");
    ASSERT_EQ(rows.size(), 1u);

    EXPECT_EQ(rows[0].timestamp, TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, "20260122"));
    EXPECT_EQ(rows[0].open_price, 105000);   // 10.5 * 10000
    EXPECT_EQ(rows[0].high_price, 112000);   // 11.2 * 10000
    EXPECT_EQ(rows[0].low_price, 103000);    // 10.3 * 10000
    EXPECT_EQ(rows[0].close_price, 108000);  // 10.8 * 10000
    EXPECT_EQ(rows[0].volume, 1234567);
    EXPECT_EQ(rows[0].amount, 987654321LL);
    EXPECT_EQ(rows[0].vwap, 0);
}

TEST_F(TushareIngestorTest, ParseResponseMultipleItems) {
    std::string json = R"({"data":{"items":[
        ["20260120",10.0,10.5,9.8,10.2,1000000,50000000.0],
        ["20260121",10.2,10.8,10.1,10.5,1200000,60000000.0],
        ["20260122",10.5,11.2,10.3,10.8,1234567,987654321.0]
    ]}})";
    auto rows = TushareIngestorTestAccessor::parse_response(*ingestor_, json, "000001.SZ");
    ASSERT_EQ(rows.size(), 3u);

    EXPECT_EQ(rows[0].timestamp, TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, "20260120"));
    EXPECT_EQ(rows[0].open_price, 100000);
    EXPECT_EQ(rows[0].close_price, 102000);
    EXPECT_EQ(rows[0].volume, 1000000);

    EXPECT_EQ(rows[1].timestamp, TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, "20260121"));
    EXPECT_EQ(rows[1].open_price, 102000);
    EXPECT_EQ(rows[1].close_price, 105000);
    EXPECT_EQ(rows[1].volume, 1200000);

    EXPECT_EQ(rows[2].timestamp, TushareIngestorTestAccessor::date_to_epoch_us(*ingestor_, "20260122"));
    EXPECT_EQ(rows[2].open_price, 105000);
    EXPECT_EQ(rows[2].close_price, 108000);
    EXPECT_EQ(rows[2].volume, 1234567);
}

TEST_F(TushareIngestorTest, ParseResponsePrecision) {
    // Float prices with 4 decimal places → fixed-point ×10000
    std::string json = R"({"data":{"items":[["20260122",10.1234,11.5678,9.9999,10.0001,500000,25000000.5]]}})";
    auto rows = TushareIngestorTestAccessor::parse_response(*ingestor_, json, "000001.SZ");
    ASSERT_EQ(rows.size(), 1u);

    EXPECT_EQ(rows[0].open_price, 101234);   // 10.1234 * 10000
    EXPECT_EQ(rows[0].high_price, 115678);   // 11.5678 * 10000
    EXPECT_EQ(rows[0].low_price, 99999);     // 9.9999 * 10000
    EXPECT_EQ(rows[0].close_price, 100001);  // 10.0001 * 10000
}

TEST_F(TushareIngestorTest, ParseResponseNoItemsKey) {
    std::string json = R"({"data":{},"code":0})";
    EXPECT_TRUE(TushareIngestorTestAccessor::parse_response(*ingestor_, json, "000001.SZ").empty());
}

TEST_F(TushareIngestorTest, ParseResponseEmptyItemsArray) {
    std::string json = R"({"data":{"items":[]}})";
    EXPECT_TRUE(TushareIngestorTestAccessor::parse_response(*ingestor_, json, "000001.SZ").empty());
}

TEST_F(TushareIngestorTest, ParseResponseZeroValues) {
    std::string json = R"({"data":{"items":[["20260122",0,0,0,0,0,0]]}})";
    auto rows = TushareIngestorTestAccessor::parse_response(*ingestor_, json, "000001.SZ");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].open_price, 0);
    EXPECT_EQ(rows[0].volume, 0);
    EXPECT_EQ(rows[0].amount, 0);
}

}  // namespace quant::ingest
