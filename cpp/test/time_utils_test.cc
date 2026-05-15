// time_utils_test.cc — Tests for Timestamp and TradingCalendar
#include "cpp/quant/infra/time_utils.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace quant::infra {
namespace {

// ============================================================
// Timestamp Tests
// ============================================================

TEST(TimestampTest, DefaultConstruct) {
    Timestamp ts;
    EXPECT_EQ(ts.unix_nanos(), 0);
}

TEST(TimestampTest, Now) {
    auto ts = Timestamp::now();
    EXPECT_GT(ts.unix_nanos(), 0);
}

TEST(TimestampTest, FromUnixSeconds) {
    auto ts = Timestamp::from_unix_seconds(1'717'200'000.0);  // Approx 2024-06-01
    EXPECT_EQ(ts.unix_seconds(), 1'717'200'000);
}

TEST(TimestampTest, FromUnixMillis) {
    auto ts = Timestamp::from_unix_millis(1'717'200'000'000);
    EXPECT_EQ(ts.unix_millis(), 1'717'200'000'000);
}

TEST(TimestampTest, FromUnixMicros) {
    auto ts = Timestamp::from_unix_micros(1'717'200'000'000'000);
    EXPECT_EQ(ts.unix_micros(), 1'717'200'000'000'000);
}

TEST(TimestampTest, Arithmetic) {
    auto ts = Timestamp::from_unix_seconds(1000.0);
    auto later = ts + std::chrono::seconds(500);
    EXPECT_EQ(later.unix_seconds(), 1500);

    auto earlier = ts - std::chrono::seconds(300);
    EXPECT_EQ(earlier.unix_seconds(), 700);

    auto diff = later - earlier;
    EXPECT_EQ(diff.count(), 800'000'000'000LL);  // 800s in ns
}

TEST(TimestampTest, Comparison) {
    auto a = Timestamp::from_unix_seconds(100.0);
    auto b = Timestamp::from_unix_seconds(200.0);

    EXPECT_LT(a, b);
    EXPECT_GT(b, a);
    EXPECT_EQ(a, a);
    EXPECT_NE(a, b);
}

TEST(TimestampTest, ToDouble) {
    auto ts = Timestamp::from_unix_seconds(1000.5);
    EXPECT_DOUBLE_EQ(ts.to_double(), 1000.5);
}

TEST(TimestampTest, DateString) {
    // 2026-05-15 00:00:00 UTC
    struct tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 5 - 1;
    tm.tm_mday = 15;
    tm.tm_isdst = -1;
    time_t tt = timegm(&tm);

    auto ts = Timestamp::from_unix_seconds(static_cast<double>(tt));
    EXPECT_EQ(ts.to_date_str(), "2026-05-15");
}

TEST(TimestampTest, NowMonotonic) {
    auto t1 = Timestamp::now();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    auto t2 = Timestamp::now();
    EXPECT_GT(t2, t1);
}

// ============================================================
// TradingCalendar Tests
// ============================================================

TEST(TradingCalendarTest, LoadDefault) {
    auto cal = TradingCalendar::load("SSE");
    ASSERT_NE(cal, nullptr);
    EXPECT_GT(cal->size(), 0);
    EXPECT_EQ(cal->name(), "SSE");
}

TEST(TradingCalendarTest, IsTradingDay) {
    auto cal = TradingCalendar::load("SSE");

    // 2026-05-15 is a Friday (should be a trading day)
    EXPECT_TRUE(cal->is_trading_day(2026, 5, 15));

    // 2026-05-16 is a Saturday (should NOT be a trading day)
    EXPECT_FALSE(cal->is_trading_day(2026, 5, 16));

    // 2026-05-17 is a Sunday (should NOT be a trading day)
    EXPECT_FALSE(cal->is_trading_day(2026, 5, 17));
}

TEST(TradingCalendarTest, NextPrevTradingDay) {
    auto cal = TradingCalendar::load("SSE");

    // Friday 2026-05-15
    struct tm tm_fri = {};
    tm_fri.tm_year = 2026 - 1900;
    tm_fri.tm_mon = 5 - 1;
    tm_fri.tm_mday = 15;
    tm_fri.tm_isdst = -1;
    time_t fri_tt = timegm(&tm_fri);
    Timestamp fri_ts = Timestamp::from_unix_seconds(static_cast<double>(fri_tt));

    // Next trading day after Friday should be Monday 2026-05-18
    auto next = cal->next_trading_day(fri_ts);
    EXPECT_EQ(next.to_date_str(), "2026-05-18");

    // Previous trading day before Friday should be Thursday 2026-05-14
    auto prev = cal->prev_trading_day(fri_ts);
    EXPECT_EQ(prev.to_date_str(), "2026-05-14");
}

TEST(TradingCalendarTest, MarketPhase) {
    auto cal = TradingCalendar::load("SSE");

    // Test during morning session on a trading day (2026-05-15 10:00 CST)
    // 10:00 CST = 02:00 UTC
    struct tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 5 - 1;
    tm.tm_mday = 15;
    tm.tm_hour = 2;  // 02:00 UTC = 10:00 CST
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t tt = timegm(&tm);

    Timestamp ts = Timestamp::from_unix_seconds(static_cast<double>(tt));
    EXPECT_EQ(cal->phase_at(ts), MarketPhase::MorningSession);
}

TEST(TradingCalendarTest, CountBetween) {
    auto cal = TradingCalendar::load("SSE");

    struct tm tm1 = {}, tm2 = {};
    tm1.tm_year = 2026 - 1900; tm1.tm_mon = 5 - 1; tm1.tm_mday = 1;
    tm2.tm_year = 2026 - 1900; tm2.tm_mon = 5 - 1; tm2.tm_mday = 31;
    tm1.tm_isdst = tm2.tm_isdst = -1;

    time_t tt1 = timegm(&tm1);
    time_t tt2 = timegm(&tm2);

    Timestamp from = Timestamp::from_unix_seconds(static_cast<double>(tt1));
    Timestamp to = Timestamp::from_unix_seconds(static_cast<double>(tt2));

    // May 2026 has ~21 trading days (31 days - weekends)
    size_t count = cal->trading_day_count(from, to);
    EXPECT_GE(count, 20);
    EXPECT_LE(count, 23);
}

// ============================================================
// Clock Tests
// ============================================================

TEST(ClockTest, SystemClock) {
    SystemClock clock;
    auto ts = clock.now();
    EXPECT_GT(ts.unix_nanos(), 0);
    EXPECT_EQ(clock.name(), "system");
}

TEST(ClockTest, SimulatedClock) {
    auto start = Timestamp::from_unix_seconds(1'000'000.0);
    SimulatedClock clock(start);
    EXPECT_EQ(clock.now(), start);
    EXPECT_EQ(clock.name(), "simulated");

    clock.advance(std::chrono::seconds(100));
    EXPECT_EQ(clock.now().unix_seconds(), 1'000'100);

    auto new_ts = Timestamp::from_unix_seconds(2'000'000.0);
    clock.set(new_ts);
    EXPECT_EQ(clock.now(), new_ts);
}

TEST(ClockTest, GlobalClock) {
    auto& c1 = global_clock();
    EXPECT_EQ(c1.name(), "system");

    auto sim_clock = std::make_shared<SimulatedClock>(
        Timestamp::from_unix_seconds(500'000.0));
    set_global_clock(sim_clock);

    EXPECT_EQ(global_clock().now().unix_seconds(), 500'000);

    // Restore system clock
    set_global_clock(std::make_shared<SystemClock>());
    EXPECT_GT(global_clock().now().unix_nanos(), 0);
}

// ============================================================
// Stopwatch Tests
// ============================================================

TEST(StopwatchTest, Elapsed) {
    Stopwatch sw;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto elapsed = sw.elapsed_millis();
    EXPECT_GE(elapsed, 5.0);
}

}  // namespace
}  // namespace quant::infra
