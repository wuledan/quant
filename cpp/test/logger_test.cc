// logger_test.cc — Tests for structured async Logger
#include "cpp/quant/infra/logging/logger.h"
#include "cpp/quant/infra/logging/log_sink.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace quant::infra {
namespace {

// ── Test sink that captures records in memory ──
class TestSink : public LogSink {
public:
    void write(const LogRecord& record) override {
        last_level = record.level;
        last_msg = record.message;
        count++;
    }

    void flush() override { flushed = true; }

    int count{0};
    bool flushed{false};
    int last_level{0};
    std::string last_msg;
};

TEST(LoggerTest, LogLevels) {
    Logger logger("test");
    auto sink = std::make_unique<TestSink>();
    auto* raw = sink.get();
    logger.add_sink(std::move(sink));

    logger.set_level(LogLevel::kDebug);
    EXPECT_EQ(logger.level(), LogLevel::kDebug);

    logger.trace("should not appear");
    logger.flush();
    EXPECT_EQ(raw->count, 0);  // filtered by level

    logger.debug("debug message");
    logger.flush();
    EXPECT_EQ(raw->count, 1);
    EXPECT_EQ(raw->last_msg, "debug message");
    EXPECT_EQ(raw->last_level, static_cast<int>(LogLevel::kDebug));

    logger.info("info message");
    logger.flush();
    EXPECT_EQ(raw->count, 2);
    EXPECT_EQ(raw->last_msg, "info message");
}

TEST(LoggerTest, AllLevels) {
    Logger logger("test");
    auto sink = std::make_unique<TestSink>();
    auto* raw = sink.get();
    logger.add_sink(std::move(sink));
    logger.set_level(LogLevel::kTrace);

    logger.trace("trace");
    logger.debug("debug");
    logger.info("info");
    logger.warn("warn");
    logger.error("error");
    
    logger.flush();

    EXPECT_EQ(raw->count, 5);
}

TEST(LoggerTest, LevelFiltering) {
    Logger logger("test");
    auto sink = std::make_unique<TestSink>();
    auto* raw = sink.get();
    logger.add_sink(std::move(sink));
    logger.set_level(LogLevel::kError);

    logger.info("info");
    logger.warn("warn");
    logger.error("error");
    
    logger.flush();

    EXPECT_EQ(raw->count, 1);
    EXPECT_EQ(raw->last_msg, "error");
}

TEST(LoggerTest, LogBuilderWithFields) {
    Logger logger("test");
    auto sink = std::make_unique<TestSink>();
    auto* raw = sink.get();
    logger.add_sink(std::move(sink));
    logger.set_level(LogLevel::kInfo);

    logger.with_fields()
        .field("symbol", "000300.SH")
        .field("price", int64_t{4325})
        .log(LogLevel::kInfo, "trade executed");
    logger.flush();

    EXPECT_EQ(raw->count, 1);
    EXPECT_EQ(raw->last_msg, "trade executed");
}

TEST(LoggerTest, MultipleSinks) {
    Logger logger("test");
    auto sink1 = std::make_unique<TestSink>();
    auto sink2 = std::make_unique<TestSink>();
    auto* raw1 = sink1.get();
    auto* raw2 = sink2.get();

    logger.add_sink(std::move(sink1));
    logger.add_sink(std::move(sink2));
    logger.info("broadcast");
    logger.flush();

    EXPECT_EQ(raw1->count, 1);
    EXPECT_EQ(raw2->count, 1);
}

TEST(LoggerTest, LoggerName) {
    Logger logger("my_trading_strategy");
    EXPECT_EQ(logger.stats().log_count, 0);
    logger.info("hello");
    logger.flush();
    EXPECT_GE(logger.stats().log_count, 1);
}

TEST(LoggerTest, RemoveSinks) {
    Logger logger("test");
    auto sink = std::make_unique<TestSink>();
    auto* raw = sink.get();
    logger.add_sink(std::move(sink));
    logger.info("before");
    logger.flush();
    EXPECT_EQ(raw->count, 1);

    logger.remove_sinks();
    logger.info("after");
    logger.flush();
    // sink was destroyed by remove_sinks; raw is dangling
    // verify the logger doesn't crash and stats still work
    EXPECT_GE(logger.stats().log_count, 1);
}

TEST(LoggerTest, Flush) {
    Logger logger("test");
    auto sink = std::make_unique<TestSink>();
    auto* raw = sink.get();
    logger.add_sink(std::move(sink));

    logger.info("test message");
    logger.flush();
    EXPECT_TRUE(raw->flushed);
}

TEST(LoggerTest, DefaultLogger) {
    auto& dl = default_logger();
    dl.set_level(LogLevel::kInfo);
    EXPECT_EQ(dl.level(), LogLevel::kInfo);
}

}  // namespace
}  // namespace quant::infra
