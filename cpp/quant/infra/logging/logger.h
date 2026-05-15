// logger.h — Structured asynchronous logger
// Supports TRACE/DEBUG/INFO/WARN/ERROR/FATAL levels,
// structured LogBuilder API, async ring buffer flush, multi-sink.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/quant/infra/logging/log_sink.h"

namespace quant::infra {

// ── Log levels ──
enum class LogLevel : uint8_t {
    kTrace = 0,
    kDebug = 1,
    kInfo  = 2,
    kWarn  = 3,
    kError = 4,
    kFatal = 5,
};

constexpr std::string_view level_name(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::kTrace: return "TRACE";
        case LogLevel::kDebug: return "DEBUG";
        case LogLevel::kInfo:  return "INFO";
        case LogLevel::kWarn:  return "WARN";
        case LogLevel::kError: return "ERROR";
        case LogLevel::kFatal: return "FATAL";
    }
    return "UNKNOWN";
}

// ── Logger ──
class Logger {
public:
    explicit Logger(std::string_view name);
    ~Logger();

    // Disable copy/move
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // ── Basic log methods ──
    void trace(std::string_view msg,
               std::source_location loc = std::source_location::current());
    void debug(std::string_view msg,
               std::source_location loc = std::source_location::current());
    void info(std::string_view msg,
              std::source_location loc = std::source_location::current());
    void warn(std::string_view msg,
              std::source_location loc = std::source_location::current());
    void error(std::string_view msg,
               std::source_location loc = std::source_location::current());
    void fatal(std::string_view msg,
               std::source_location loc = std::source_location::current());

    // ── Structured logging builder ──
    class LogBuilder {
    public:
        LogBuilder(Logger* logger) : logger_(logger) {}

        LogBuilder& field(std::string_view key, std::string_view value);
        LogBuilder& field(std::string_view key, int64_t value);
        LogBuilder& field(std::string_view key, double value);
        LogBuilder& field(std::string_view key, bool value);

        void log(LogLevel level, std::string_view msg,
                 std::source_location loc = std::source_location::current());

    private:
        Logger* logger_;
        std::vector<std::pair<std::string, std::string>> fields_;
    };

    LogBuilder with_fields() { return LogBuilder(this); }

    // ── Configuration ──
    void set_level(LogLevel level) { min_level_.store(level, std::memory_order_relaxed); }
    LogLevel level() const noexcept {
        return min_level_.load(std::memory_order_relaxed);
    }

    void add_sink(std::unique_ptr<LogSink> sink);
    void remove_sinks();

    // ── Flush ──
    void flush();

    // ── Metrics ──
    struct LoggerStats {
        uint64_t log_count{0};
        uint64_t dropped_count{0};
    };
    LoggerStats stats() const;

private:
    void log_impl(LogLevel level, std::string_view msg,
                  std::string_view extra_json,
                  std::source_location loc);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<LogLevel> min_level_{LogLevel::kTrace};
};

// ── Global default logger ──
Logger& default_logger();
void set_default_logger(std::unique_ptr<Logger> logger);

}  // namespace quant::infra
