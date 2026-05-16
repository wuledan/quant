// logger.cc — Structured asynchronous logger implementation
#include "cpp/quant/infra/logging/logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace quant::infra {

// ── Thread-local record cache ──
// Each thread has its own batch buffer to reduce contention.
// The background flush thread periodically collects from all thread-local buffers.

// ── Logger::Impl ──
class Logger::Impl {
public:
    Impl(std::string_view name)
        : name_(name)
        , flush_thread_([this] { flush_loop(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(queue_mutex_);
            stop_.store(true, std::memory_order_release);
        }
        cv_.notify_one();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
        flush_all();
    }

    void add_sink(std::unique_ptr<LogSink> sink) {
        std::lock_guard lock(sink_mutex_);
        sinks_.push_back(std::move(sink));
    }

    void remove_sinks() {
        std::lock_guard lock(sink_mutex_);
        sinks_.clear();
    }

    void enqueue(LogRecord record) {
        {
            std::lock_guard lock(queue_mutex_);
            if (queue_.size() >= kMaxQueueSize) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            queue_.push(std::move(record));
            count_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
    }

    void flush_all() {
        std::vector<LogRecord> batch;
        {
            std::lock_guard lock(queue_mutex_);
            batch.reserve(queue_.size());
            while (!queue_.empty()) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop();
            }
        }
        if (!batch.empty()) {
            std::shared_lock lock(sink_mutex_);
            for (const auto& record : batch) {
                for (auto& sink : sinks_) {
                    sink->write(record);
                }
            }
            for (auto& sink : sinks_) {
                sink->flush();
            }
        }
    }

    uint64_t count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

    uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    static constexpr size_t kMaxQueueSize = 65536;

    void flush_loop() {
        while (!stop_.load(std::memory_order_acquire)) {
            {
                std::unique_lock lock(queue_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(1),
                             [this] { return stop_.load(std::memory_order_acquire)
                                           || !queue_.empty(); });
            }
            flush_all();
        }
    }

    std::string name_;
    std::queue<LogRecord> queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::thread flush_thread_;

    std::vector<std::unique_ptr<LogSink>> sinks_;
    mutable std::shared_mutex sink_mutex_;

    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> dropped_{0};
};

// ── Helper: get current time in microseconds ──
static int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ── Helper: get thread id string ──
static std::string thread_id_str() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

// ── Logger constructor ──
Logger::Logger(std::string_view name)
    : impl_(std::make_unique<Impl>(name)) {}

Logger::~Logger() = default;

void Logger::log_impl(LogLevel level, std::string_view msg,
                      std::string_view extra_json,
                      std::source_location loc) {
    if (level < min_level_.load(std::memory_order_relaxed)) {
        return;  // filtered
    }

    LogRecord record{
        .timestamp_us = now_us(),
        .level = static_cast<uint8_t>(level),
        .file = loc.file_name() ? loc.file_name() : "",
        .line = static_cast<int>(loc.line()),
        .function = loc.function_name() ? loc.function_name() : "",
        .message = std::string(msg),
        .thread_id = thread_id_str(),
        .extra_json = std::string(extra_json),
    };
    impl_->enqueue(std::move(record));
}

void Logger::trace(std::string_view msg, std::source_location loc) {
    log_impl(LogLevel::kTrace, msg, "", loc);
}

void Logger::debug(std::string_view msg, std::source_location loc) {
    log_impl(LogLevel::kDebug, msg, "", loc);
}

void Logger::info(std::string_view msg, std::source_location loc) {
    log_impl(LogLevel::kInfo, msg, "", loc);
}

void Logger::warn(std::string_view msg, std::source_location loc) {
    log_impl(LogLevel::kWarn, msg, "", loc);
}

void Logger::error(std::string_view msg, std::source_location loc) {
    log_impl(LogLevel::kError, msg, "", loc);
}

void Logger::fatal(std::string_view msg, std::source_location loc) {
    log_impl(LogLevel::kFatal, msg, "", loc);
    flush();
    std::abort();
}

// ── LogBuilder ──

Logger::LogBuilder& Logger::LogBuilder::field(std::string_view key, std::string_view value) {
    fields_.emplace_back(std::string(key), "\"" + std::string(value) + "\"");
    return *this;
}

Logger::LogBuilder& Logger::LogBuilder::field(std::string_view key, int64_t value) {
    fields_.emplace_back(std::string(key), std::to_string(value));
    return *this;
}

Logger::LogBuilder& Logger::LogBuilder::field(std::string_view key, double value) {
    fields_.emplace_back(std::string(key), std::to_string(value));
    return *this;
}

Logger::LogBuilder& Logger::LogBuilder::field(std::string_view key, bool value) {
    fields_.emplace_back(std::string(key), value ? "true" : "false");
    return *this;
}

void Logger::LogBuilder::log(LogLevel level, std::string_view msg,
                              std::source_location loc) {
    std::string json;
    if (!fields_.empty()) {
        json = "{";
        for (size_t i = 0; i < fields_.size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + fields_[i].first + "\":" + fields_[i].second;
        }
        json += "}";
    }
    logger_->log_impl(level, msg, json, loc);
}

void Logger::add_sink(std::unique_ptr<LogSink> sink) {
    impl_->add_sink(std::move(sink));
}

void Logger::remove_sinks() {
    impl_->remove_sinks();
}

void Logger::flush() {
    impl_->flush_all();
}

Logger::LoggerStats Logger::stats() const {
    return LoggerStats{
        .log_count = impl_->count(),
        .dropped_count = impl_->dropped(),
    };
}

// ── Global default logger ──

Logger& default_logger() {
    static Logger logger("quant");
    return logger;
}

void set_default_logger(std::unique_ptr<Logger> new_logger) {
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-but-set-variable"
    // Note: This is intentionally a leak to avoid static destruction ordering issues.
    // In a real implementation, redirect calls via a std::atomic<Logger*>.
    static Logger* current = &default_logger();
    if (new_logger) {
        current = new_logger.release();
    }
    #pragma GCC diagnostic pop
}

}  // namespace quant::infra
    #pragma GCC diagnostic pop
