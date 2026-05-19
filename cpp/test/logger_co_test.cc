// logger_co_test.cc — Tests for Logger coroutine async flush
#include "logger.h"
#include "log_sink.h"
#include "work_stealing_executor.h"
#include "coroutine.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <folly/coro/BlockingWait.h>

using namespace quant::infra;

namespace {

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

}

TEST(LoggerCoTest, SyncLoggingStillWorks) {
    Logger logger("test");
    auto sink = std::make_unique<TestSink>();
    auto* raw = sink.get();
    logger.add_sink(std::move(sink));
    logger.set_level(LogLevel::kTrace);

    logger.debug("debug message");
    logger.flush();

    EXPECT_EQ(raw->count, 1);
    EXPECT_EQ(raw->last_msg, "debug message");
}

TEST(LoggerCoTest, LogLevelFiltering) {
    Logger logger("test");
    auto sink = std::make_unique<TestSink>();
    auto* raw = sink.get();
    logger.add_sink(std::move(sink));
    logger.set_level(LogLevel::kError);

    logger.info("info");
    logger.error("error");
    logger.flush();

    EXPECT_EQ(raw->count, 1);
    EXPECT_EQ(raw->last_msg, "error");
}

TEST(LoggerCoTest, LogBuilderWithFields) {
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

TEST(LoggerCoTest, MultipleSinks) {
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
