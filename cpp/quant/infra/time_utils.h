// time_utils.h — Timestamp, TradingCalendar, and Clock abstractions
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace quant::infra {

// ── Nanosecond-precision timestamp ──
class Timestamp {
public:
    constexpr Timestamp() noexcept : ns_(0) {}
    explicit constexpr Timestamp(int64_t nanoseconds) noexcept : ns_(nanoseconds) {}

    // Factory methods
    static Timestamp now() noexcept;
    static Timestamp from_unix_seconds(double sec) noexcept;
    static Timestamp from_unix_millis(int64_t ms) noexcept;
    static Timestamp from_unix_micros(int64_t us) noexcept;
    static Timestamp from_unix_nanos(int64_t ns) noexcept;
    static Timestamp from_iso8601(std::string_view str);

    // ── Conversions ──
    constexpr int64_t unix_nanos() const noexcept { return ns_; }
    constexpr int64_t unix_micros() const noexcept { return ns_ / 1'000; }
    constexpr int64_t unix_millis() const noexcept { return ns_ / 1'000'000; }
    constexpr int64_t unix_seconds() const noexcept { return ns_ / 1'000'000'000; }
    constexpr double to_double() const noexcept {
        return static_cast<double>(ns_) / 1'000'000'000.0;
    }

    std::string to_iso8601() const;           // "2026-05-15T09:30:00.123456789+08:00"
    std::string to_date_str() const;          // "2026-05-15"
    std::string to_time_str() const;          // "09:30:00.123456789"

    // ── Arithmetic ──
    constexpr Timestamp operator+(std::chrono::nanoseconds dur) const noexcept {
        return Timestamp{ns_ + dur.count()};
    }
    constexpr Timestamp operator-(std::chrono::nanoseconds dur) const noexcept {
        return Timestamp{ns_ - dur.count()};
    }
    constexpr std::chrono::nanoseconds operator-(Timestamp other) const noexcept {
        return std::chrono::nanoseconds{ns_ - other.ns_};
    }
    constexpr auto operator<=>(const Timestamp&) const noexcept = default;

    // ── Chrono interop ──
    using clock_type = std::chrono::system_clock;
    std::chrono::system_clock::time_point to_time_point() const noexcept;
    static Timestamp from_time_point(
        const std::chrono::system_clock::time_point& tp) noexcept;

private:
    int64_t ns_;  // Unix epoch nanoseconds
};

// ── Time interval ──
struct TimeInterval {
    Timestamp start;
    Timestamp end;

    bool contains(Timestamp ts) const noexcept {
        return ts >= start && ts <= end;
    }

    std::chrono::nanoseconds duration() const noexcept {
        return end - start;
    }
};

// ── Market phases ──
enum class MarketPhase {
    PreMarket,          // Pre-market call auction (09:15-09:25)
    OpenAuction,        // Opening call auction (09:25-09:30)
    MorningSession,     // Morning continuous trading (09:30-11:30)
    LunchBreak,         // Lunch break (11:30-13:00)
    AfternoonSession,   // Afternoon continuous trading (13:00-14:57)
    ClosingAuction,     // Closing call auction (14:57-15:00)
    AfterHours,         // After hours (15:00+)
    Closed,             // Non-trading day
};

// ── Trading calendar ──
// Helper struct to enable make_shared with private constructor
struct TradingCalendarTag {};

class TradingCalendar {
public:
    // Constructor for make_shared — public but requires TradingCalendarTag
    explicit TradingCalendar(TradingCalendarTag) : TradingCalendar() {}

    // ── Load calendar data ──
    static std::shared_ptr<TradingCalendar> load(
        std::string_view calendar_name = "SSE");

    // ── Build from date list ──
    static std::shared_ptr<TradingCalendar> from_dates(
        std::vector<Timestamp> trading_days,
        std::string_view calendar_name = "custom");

    // ── Trading day queries ──
    bool is_trading_day(const Timestamp& ts) const noexcept;
    bool is_trading_day(int year, int month, int day) const noexcept;

    // ── Market phase at a given timestamp ──
    MarketPhase phase_at(const Timestamp& ts) const noexcept;

    // ── Next/previous trading day ──
    Timestamp next_trading_day(const Timestamp& ts) const noexcept;
    Timestamp prev_trading_day(const Timestamp& ts) const noexcept;

    // ── Trading day intervals ──
    TimeInterval morning_session(int year, int month, int day) const noexcept;
    TimeInterval afternoon_session(int year, int month, int day) const noexcept;
    TimeInterval trading_hours(int year, int month, int day) const noexcept;

    // ── Statistics ──
    size_t trading_day_count(Timestamp from, Timestamp to) const noexcept;
    std::vector<Timestamp> trading_days(Timestamp from, Timestamp to) const;

    // ── Calendar info ──
    std::string_view name() const noexcept { return name_; }
    size_t size() const noexcept { return trading_days_.size(); }

private:
    TradingCalendar() = default;

    int64_t to_key(int year, int month, int day) const noexcept {
        return static_cast<int64_t>(year) * 10000
             + static_cast<int64_t>(month) * 100
             + static_cast<int64_t>(day);
    }

    std::string name_ = "SSE";
    std::vector<Timestamp> trading_days_;
    std::unordered_set<int64_t> day_set_;  // YYYYMMDD keys for O(1) lookup
};

// ── Clock interface (injectable for testing) ──
class Clock {
public:
    virtual ~Clock() = default;
    virtual Timestamp now() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
};

class SystemClock : public Clock {
public:
    Timestamp now() const noexcept override { return Timestamp::now(); }
    std::string_view name() const noexcept override { return "system"; }
};

class SimulatedClock : public Clock {
public:
    explicit SimulatedClock(Timestamp start_time) : current_(start_time) {}
    Timestamp now() const noexcept override { return current_; }
    std::string_view name() const noexcept override { return "simulated"; }
    void advance(std::chrono::nanoseconds dur) noexcept { current_ = current_ + dur; }
    void set(Timestamp ts) noexcept { current_ = ts; }

private:
    Timestamp current_;
};

// ── Global clock ──
void set_global_clock(std::shared_ptr<Clock> clock);
Clock& global_clock();

// ── Utility timing functions ──
class Stopwatch {
public:
    Stopwatch() : start_(Timestamp::now()) {}
    explicit Stopwatch(Timestamp start) : start_(start) {}

    std::chrono::nanoseconds elapsed() const noexcept {
        return Timestamp::now() - start_;
    }

    double elapsed_seconds() const noexcept {
        return static_cast<double>(elapsed().count()) / 1'000'000'000.0;
    }

    double elapsed_millis() const noexcept {
        return static_cast<double>(elapsed().count()) / 1'000'000.0;
    }

    void reset() noexcept { start_ = Timestamp::now(); }

private:
    Timestamp start_;
};

}  // namespace quant::infra
