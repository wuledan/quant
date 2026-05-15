// time_utils.cc — Timestamp and TradingCalendar implementation
#include "cpp/quant/infra/time_utils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace quant::infra {

// ============================================================
// Timestamp implementation
// ============================================================

Timestamp Timestamp::now() noexcept {
    auto tp = std::chrono::system_clock::now();
    auto duration = tp.time_since_epoch();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    return Timestamp{nanos};
}

Timestamp Timestamp::from_unix_seconds(double sec) noexcept {
    return Timestamp{static_cast<int64_t>(sec * 1'000'000'000.0)};
}

Timestamp Timestamp::from_unix_millis(int64_t ms) noexcept {
    return Timestamp{ms * 1'000'000};
}

Timestamp Timestamp::from_unix_micros(int64_t us) noexcept {
    return Timestamp{us * 1'000};
}

Timestamp Timestamp::from_unix_nanos(int64_t ns) noexcept {
    return Timestamp{ns};
}

Timestamp Timestamp::from_iso8601(std::string_view str) {
    // Parse ISO 8601: "2026-05-15T09:30:00.123456789+08:00"
    // Or simpler: "2026-05-15 09:30:00"
    struct tm tm = {};
    int micro_secs = 0;
    int tz_hour = 0, tz_min = 0;
    char tz_sign = '+';

    // Handle space separator
    std::string copy(str);
    if (copy.find('T') == std::string::npos && copy.find(' ') != std::string::npos) {
        copy[copy.find(' ')] = 'T';
    }

    auto* result = strptime(copy.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (result == nullptr) {
        throw std::invalid_argument("Failed to parse ISO 8601 timestamp: " + std::string(str));
    }

    // Parse fractional seconds
    if (*result == '.') {
        ++result;
        char frac[10] = {};
        int i = 0;
        while (i < 9 && result[i] && std::isdigit(static_cast<unsigned char>(result[i]))) {
            frac[i] = result[i];
            ++i;
        }
        if (i > 0) {
            micro_secs = std::atoi(frac);
            // Pad to 9 digits (nanoseconds)
            while (i < 9) { frac[i] = '0'; ++i; }
            micro_secs = std::atoi(frac);
        }
        result += i;
    }

    // Parse timezone
    if (*result == '+' || *result == '-') {
        tz_sign = *result;
        ++result;
        tz_hour = (result[0] - '0') * 10 + (result[1] - '0');
        tz_min = (result[3] - '0') * 10 + (result[4] - '0');
    } else if (*result == 'Z') {
        tz_sign = '+';
        tz_hour = 0;
        tz_min = 0;
    }

    // Convert to time_t (UTC)
    tm.tm_isdst = -1;
    time_t tt = timegm(&tm);  // Treat as UTC initially

    // Apply timezone offset (subtract because we want UTC)
    int tz_offset_seconds = tz_hour * 3600 + tz_min * 60;
    if (tz_sign == '-') tz_offset_seconds = -tz_offset_seconds;
    tt -= tz_offset_seconds;

    int64_t nanos = static_cast<int64_t>(tt) * 1'000'000'000 + micro_secs;
    return Timestamp{nanos};
}

std::string Timestamp::to_iso8601() const {
    time_t sec = static_cast<time_t>(ns_ / 1'000'000'000);
    int64_t nsec = ns_ % 1'000'000'000;
    if (nsec < 0) { nsec += 1'000'000'000; sec -= 1; }

    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);

    char date_part[64];
    strftime(date_part, sizeof(date_part), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    // Add nanoseconds and timezone
    char result[128];
    snprintf(result, sizeof(result), "%s.%09ld+08:00", date_part,
             static_cast<long>(nsec));

    return std::string(result);
}

std::string Timestamp::to_date_str() const {
    time_t sec = static_cast<time_t>(ns_ / 1'000'000'000);
    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);

    char result[16];
    strftime(result, sizeof(result), "%Y-%m-%d", &tm_buf);
    return std::string(result);
}

std::string Timestamp::to_time_str() const {
    time_t sec = static_cast<time_t>(ns_ / 1'000'000'000);
    int64_t nsec = ns_ % 1'000'000'000;
    if (nsec < 0) { nsec += 1'000'000'000; sec -= 1; }

    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);

    char time_part[64];
    strftime(time_part, sizeof(time_part), "%H:%M:%S", &tm_buf);

    char result[80];
    snprintf(result, sizeof(result), "%s.%09ld", time_part,
             static_cast<long>(nsec));
    return std::string(result);
}

std::chrono::system_clock::time_point Timestamp::to_time_point() const noexcept {
    auto dur = std::chrono::nanoseconds(ns_);
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(dur));
}

Timestamp Timestamp::from_time_point(
    const std::chrono::system_clock::time_point& tp) noexcept {
    auto dur = tp.time_since_epoch();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
    return Timestamp{nanos};
}

// ============================================================
// TradingCalendar implementation
// ============================================================

std::shared_ptr<TradingCalendar> TradingCalendar::load(
    std::string_view calendar_name) {
    auto cal = std::make_shared<TradingCalendar>(TradingCalendarTag{});
    cal->name_ = calendar_name;

    // For now, generate a basic calendar with weekends off
    // In production, this would load from a CSV/data file
    // A-share market: Monday to Friday, excluding public holidays

    // Generate all weekdays from 2020 to 2030 as a reasonable default
    for (int year = 2020; year <= 2030; ++year) {
        for (int month = 1; month <= 12; ++month) {
            int days_in_month = 31;
            if (month == 2) {
                bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
                days_in_month = leap ? 29 : 28;
            } else if (month == 4 || month == 6 || month == 9 || month == 11) {
                days_in_month = 30;
            }

            for (int day = 1; day <= days_in_month; ++day) {
                // Check if weekday
                struct tm tm = {};
                tm.tm_year = year - 1900;
                tm.tm_mon = month - 1;
                tm.tm_mday = day;
                tm.tm_isdst = -1;
                time_t tt = timegm(&tm);
                struct tm* result = gmtime_r(&tt, &tm);
                if (result && result->tm_wday != 0 && result->tm_wday != 6) {
                    Timestamp ts(static_cast<int64_t>(tt) * 1'000'000'000);
                    cal->trading_days_.push_back(ts);
                    cal->day_set_.insert(cal->to_key(year, month, day));
                }
            }
        }
    }

    // Sort just in case
    std::sort(cal->trading_days_.begin(), cal->trading_days_.end());

    return cal;
}

std::shared_ptr<TradingCalendar> TradingCalendar::from_dates(
    std::vector<Timestamp> dates,
    std::string_view calendar_name) {
    auto cal = std::make_shared<TradingCalendar>(TradingCalendarTag{});
    cal->name_ = calendar_name;
    cal->trading_days_ = std::move(dates);

    std::sort(cal->trading_days_.begin(), cal->trading_days_.end());

    for (const auto& ts : cal->trading_days_) {
        time_t sec = ts.unix_seconds();
        struct tm tm_buf;
        gmtime_r(&sec, &tm_buf);
        int64_t key = cal->to_key(
            tm_buf.tm_year + 1900,
            tm_buf.tm_mon + 1,
            tm_buf.tm_mday);
        cal->day_set_.insert(key);
    }

    return cal;
}

bool TradingCalendar::is_trading_day(const Timestamp& ts) const noexcept {
    time_t sec = ts.unix_seconds();
    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);
    return is_trading_day(
        tm_buf.tm_year + 1900,
        tm_buf.tm_mon + 1,
        tm_buf.tm_mday);
}

bool TradingCalendar::is_trading_day(int year, int month, int day) const noexcept {
    return day_set_.contains(to_key(year, month, day));
}

MarketPhase TradingCalendar::phase_at(const Timestamp& ts) const noexcept {
    time_t sec = ts.unix_seconds();
    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);

    int year = tm_buf.tm_year + 1900;
    int month = tm_buf.tm_mon + 1;
    int day = tm_buf.tm_mday;

    if (!is_trading_day(year, month, day)) {
        return MarketPhase::Closed;
    }

    // Time within day in seconds (UTC+8 / CST)
    // We work in UTC internally, so add 8 hours for CST
    int seconds_since_midnight_cst = (tm_buf.tm_hour + 8) * 3600
                                   + tm_buf.tm_min * 60
                                   + tm_buf.tm_sec;
    // Wrap around if past midnight UTC
    if (seconds_since_midnight_cst >= 86400) {
        seconds_since_midnight_cst -= 86400;
    }

    // A-share market phases (CST):
    // Pre-market:  09:15-09:25
    // Open auction:09:25-09:30
    // Morning:     09:30-11:30
    // Lunch break: 11:30-13:00
    // Afternoon:   13:00-14:57
    // Close auc.:  14:57-15:00
    // After hours: 15:00+

    if (seconds_since_midnight_cst >= 9*3600+15*60 &&
        seconds_since_midnight_cst < 9*3600+25*60) {
        return MarketPhase::PreMarket;
    }
    if (seconds_since_midnight_cst >= 9*3600+25*60 &&
        seconds_since_midnight_cst < 9*3600+30*60) {
        return MarketPhase::OpenAuction;
    }
    if (seconds_since_midnight_cst >= 9*3600+30*60 &&
        seconds_since_midnight_cst < 11*3600+30*60) {
        return MarketPhase::MorningSession;
    }
    if (seconds_since_midnight_cst >= 11*3600+30*60 &&
        seconds_since_midnight_cst < 13*3600) {
        return MarketPhase::LunchBreak;
    }
    if (seconds_since_midnight_cst >= 13*3600 &&
        seconds_since_midnight_cst < 14*3600+57*60) {
        return MarketPhase::AfternoonSession;
    }
    if (seconds_since_midnight_cst >= 14*3600+57*60 &&
        seconds_since_midnight_cst < 15*3600) {
        return MarketPhase::ClosingAuction;
    }
    return MarketPhase::AfterHours;
}

Timestamp TradingCalendar::next_trading_day(const Timestamp& ts) const noexcept {
    if (trading_days_.empty()) return ts;

    auto it = std::upper_bound(trading_days_.begin(), trading_days_.end(), ts);
    if (it == trading_days_.end()) {
        return trading_days_.back();
    }
    return *it;
}

Timestamp TradingCalendar::prev_trading_day(const Timestamp& ts) const noexcept {
    if (trading_days_.empty()) return ts;

    auto it = std::lower_bound(trading_days_.begin(), trading_days_.end(), ts);
    if (it == trading_days_.begin()) {
        return trading_days_.front();
    }
    --it;
    return *it;
}

TimeInterval TradingCalendar::morning_session(int year, int month, int day) const noexcept {
    struct tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_isdst = -1;

    // 09:30 CST = 01:30 UTC
    tm.tm_hour = 1; tm.tm_min = 30; tm.tm_sec = 0;
    time_t start_sec = timegm(&tm);

    // 11:30 CST = 03:30 UTC
    tm.tm_hour = 3; tm.tm_min = 30; tm.tm_sec = 0;
    time_t end_sec = timegm(&tm);

    return {Timestamp{start_sec * 1'000'000'000}, Timestamp{end_sec * 1'000'000'000}};
}

TimeInterval TradingCalendar::afternoon_session(int year, int month, int day) const noexcept {
    struct tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_isdst = -1;

    // 13:00 CST = 05:00 UTC
    tm.tm_hour = 5; tm.tm_min = 0; tm.tm_sec = 0;
    time_t start_sec = timegm(&tm);

    // 15:00 CST = 07:00 UTC
    tm.tm_hour = 7; tm.tm_min = 0; tm.tm_sec = 0;
    time_t end_sec = timegm(&tm);

    return {Timestamp{start_sec * 1'000'000'000}, Timestamp{end_sec * 1'000'000'000}};
}

TimeInterval TradingCalendar::trading_hours(int year, int month, int day) const noexcept {
    auto morning = morning_session(year, month, day);
    auto afternoon = afternoon_session(year, month, day);
    return {morning.start, afternoon.end};
}

size_t TradingCalendar::trading_day_count(Timestamp from, Timestamp to) const noexcept {
    if (trading_days_.empty()) return 0;
    auto from_it = std::lower_bound(trading_days_.begin(), trading_days_.end(), from);
    auto to_it = std::upper_bound(trading_days_.begin(), trading_days_.end(), to);
    return static_cast<size_t>(std::distance(from_it, to_it));
}

std::vector<Timestamp> TradingCalendar::trading_days(Timestamp from, Timestamp to) const {
    if (trading_days_.empty()) return {};
    auto from_it = std::lower_bound(trading_days_.begin(), trading_days_.end(), from);
    auto to_it = std::upper_bound(trading_days_.begin(), trading_days_.end(), to);
    return {from_it, to_it};
}

// ============================================================
// Global clock implementation
// ============================================================

namespace {
std::shared_ptr<Clock>& global_clock_instance() {
    static std::shared_ptr<Clock> instance = std::make_shared<SystemClock>();
    return instance;
}
}

void set_global_clock(std::shared_ptr<Clock> clock) {
    global_clock_instance() = std::move(clock);
}

Clock& global_clock() {
    return *global_clock_instance();
}

}  // namespace quant::infra
