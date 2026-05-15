// metrics.h — Lightweight metrics collection
// Counter, Histogram, Gauge with Prometheus text format export
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace quant::infra {

// ── Counter: monotonic increment-only counter ──
class Counter {
public:
    explicit Counter(std::string_view name, std::string_view help = "")
        : name_(name), help_(help) {}

    void increment(int64_t value = 1) noexcept {
        value_.fetch_add(value, std::memory_order_relaxed);
    }

    int64_t value() const noexcept {
        return value_.load(std::memory_order_acquire);
    }

    void reset() noexcept {
        value_.store(0, std::memory_order_release);
    }

    const std::string& name() const noexcept { return name_; }
    const std::string& help() const noexcept { return help_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<int64_t> value_{0};
};

// ── Gauge: point-in-time value that can go up and down ──
class Gauge {
public:
    explicit Gauge(std::string_view name, std::string_view help = "")
        : name_(name), help_(help) {}

    void set(double value) noexcept {
        value_.store(value, std::memory_order_release);
    }

    void increment(double delta = 1.0) noexcept {
        double expected = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(expected, expected + delta,
                                             std::memory_order_acq_rel)) {}
    }

    void decrement(double delta = 1.0) noexcept {
        increment(-delta);
    }

    double value() const noexcept {
        return value_.load(std::memory_order_acquire);
    }

    const std::string& name() const noexcept { return name_; }
    const std::string& help() const noexcept { return help_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<double> value_{0.0};
};

// ── Histogram: bucketed observation counter with percentiles ──
class Histogram {
public:
    Histogram(std::string_view name,
              std::vector<double> boundaries,
              std::string_view help = "");
    ~Histogram();

    void observe(double value) noexcept;

    uint64_t count() const noexcept;
    double sum() const noexcept;
    double min() const noexcept;
    double max() const noexcept;
    double mean() const noexcept;

    double percentile(double p) const;

    const std::string& name() const noexcept { return name_; }
    const std::string& help() const noexcept { return help_; }
    const std::vector<double>& boundaries() const noexcept { return boundaries_; }

    std::vector<uint64_t> bucket_counts() const;

private:
    std::string name_;
    std::string help_;
    std::vector<double> boundaries_;
    std::vector<std::atomic<uint64_t>> buckets_;
    std::atomic<uint64_t> count_{0};
    std::atomic<double> sum_{0.0};
    std::atomic<double> min_{0.0};
    std::atomic<double> max_{0.0};
};

// ── MetricsRegistry: central registry for all metrics ──
class MetricsRegistry {
public:
    static MetricsRegistry& instance();

    Counter& counter(std::string_view name, std::string_view help = "");
    Gauge& gauge(std::string_view name, std::string_view help = "");
    Histogram& histogram(std::string_view name,
                          std::vector<double> boundaries,
                          std::string_view help = "");

    std::string to_prometheus() const;
    std::string to_json() const;

    const std::vector<std::unique_ptr<Counter>>& counters() const noexcept { return counters_; }
    const std::vector<std::unique_ptr<Gauge>>& gauges() const noexcept { return gauges_; }
    const std::vector<std::unique_ptr<Histogram>>& histograms() const noexcept { return histograms_; }

private:
    MetricsRegistry() = default;
    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

    std::vector<std::unique_ptr<Counter>> counters_;
    std::vector<std::unique_ptr<Gauge>> gauges_;
    std::vector<std::unique_ptr<Histogram>> histograms_;
    mutable std::shared_mutex mutex_;
};

}  // namespace quant::infra
