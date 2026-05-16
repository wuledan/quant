// metrics.cc — Metrics implementation
#include "cpp/quant/infra/logging/metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <sstream>

namespace quant::infra {

// ========================================================================
// Histogram
// ========================================================================

Histogram::Histogram(std::string_view name,
                      std::vector<double> boundaries,
                      std::string_view help)
    : name_(name)
    , help_(help)
    , boundaries_(std::move(boundaries))
    , buckets_(boundaries_.size() + 1)
{
    min_.store(std::numeric_limits<double>::max(), std::memory_order_relaxed);
    max_.store(std::numeric_limits<double>::lowest(), std::memory_order_relaxed);
}

Histogram::~Histogram() = default;

void Histogram::observe(double value) noexcept {
    count_.fetch_add(1, std::memory_order_relaxed);
    // CAS loop for atomic sum update
    double expected = sum_.load(std::memory_order_relaxed);
    while (!sum_.compare_exchange_weak(expected, expected + value,
                                        std::memory_order_acq_rel)) {}

    // Track min/max
    double prev_min = min_.load(std::memory_order_relaxed);
    while (value < prev_min && !min_.compare_exchange_weak(prev_min, value,
                                                           std::memory_order_acq_rel)) {}
    double prev_max = max_.load(std::memory_order_relaxed);
    while (value > prev_max && !max_.compare_exchange_weak(prev_max, value,
                                                           std::memory_order_acq_rel)) {}

    // Find bucket
    for (size_t i = 0; i < boundaries_.size(); ++i) {
        if (value <= boundaries_[i]) {
            buckets_[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // +Inf bucket
    buckets_.back().fetch_add(1, std::memory_order_relaxed);
}

uint64_t Histogram::count() const noexcept {
    return count_.load(std::memory_order_acquire);
}

double Histogram::sum() const noexcept {
    return sum_.load(std::memory_order_acquire);
}

double Histogram::min() const noexcept {
    double v = min_.load(std::memory_order_acquire);
    return v == std::numeric_limits<double>::max() ? 0.0 : v;
}

double Histogram::max() const noexcept {
    double v = max_.load(std::memory_order_acquire);
    return v == std::numeric_limits<double>::lowest() ? 0.0 : v;
}

double Histogram::mean() const noexcept {
    auto c = count_.load(std::memory_order_acquire);
    return c > 0 ? sum_.load(std::memory_order_acquire) / static_cast<double>(c) : 0.0;
}

double Histogram::percentile(double p) const {
    auto total = count_.load(std::memory_order_acquire);
    if (total == 0) return 0.0;

    double target = total * p / 100.0;
    uint64_t cumulative = 0;

    for (size_t i = 0; i < boundaries_.size(); ++i) {
        cumulative += buckets_[i].load(std::memory_order_acquire);
        if (cumulative >= target) {
            // Linear interpolation within the bucket
            if (i == 0) {
                return boundaries_[0] * (static_cast<double>(target) / cumulative);
            }
            double prev_boundary = (i > 0) ? boundaries_[i - 1] : 0.0;
            double prev_cumul = cumulative - buckets_[i].load(std::memory_order_acquire);
            double ratio = (target - prev_cumul)
                         / buckets_[i].load(std::memory_order_acquire);
            return prev_boundary + (boundaries_[i] - prev_boundary) * ratio;
        }
    }
    return max();
}

std::vector<uint64_t> Histogram::bucket_counts() const {
    std::vector<uint64_t> counts(buckets_.size());
    for (size_t i = 0; i < buckets_.size(); ++i) {
        counts[i] = buckets_[i].load(std::memory_order_acquire);
    }
    return counts;
}

// ========================================================================
// MetricsRegistry
// ========================================================================

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry registry;
    return registry;
}

Counter& MetricsRegistry::counter(std::string_view name, std::string_view help) {
    std::unique_lock lock(mutex_);
    counters_.push_back(std::make_unique<Counter>(name, help));
    return *counters_.back();
}

Gauge& MetricsRegistry::gauge(std::string_view name, std::string_view help) {
    std::unique_lock lock(mutex_);
    gauges_.push_back(std::make_unique<Gauge>(name, help));
    return *gauges_.back();
}

Histogram& MetricsRegistry::histogram(std::string_view name,
                                        std::vector<double> boundaries,
                                        std::string_view help) {
    std::unique_lock lock(mutex_);
    histograms_.push_back(
        std::make_unique<Histogram>(name, std::move(boundaries), help));
    return *histograms_.back();
}

std::string MetricsRegistry::to_prometheus() const {
    std::ostringstream oss;
    std::shared_lock lock(mutex_);

    for (const auto& c : counters_) {
        oss << "# HELP " << c->name() << " " << c->help() << "\n";
        oss << "# TYPE " << c->name() << " counter\n";
        oss << c->name() << " " << c->value() << "\n";
    }

    for (const auto& g : gauges_) {
        oss << "# HELP " << g->name() << " " << g->help() << "\n";
        oss << "# TYPE " << g->name() << " gauge\n";
        oss << g->name() << " " << g->value() << "\n";
    }

    for (const auto& h : histograms_) {
        oss << "# HELP " << h->name() << " " << h->help() << "\n";
        oss << "# TYPE " << h->name() << " histogram\n";
        auto counts = h->bucket_counts();
        double cumulative = 0;
        oss << h->name() << "_sum " << h->sum() << "\n";
        oss << h->name() << "_count " << h->count() << "\n";
        for (size_t i = 0; i < h->boundaries().size(); ++i) {
            cumulative += counts[i];
            oss << h->name() << "_bucket{le=\"" << h->boundaries()[i] << "\"} "
                << static_cast<uint64_t>(cumulative) << "\n";
        }
        cumulative += counts.back();
        oss << h->name() << "_bucket{le=\"+Inf\"} "
            << static_cast<uint64_t>(cumulative) << "\n";
    }

    return oss.str();
}

std::string MetricsRegistry::to_json() const {
    std::ostringstream oss;
    std::shared_lock lock(mutex_);

    oss << "{\"metrics\":{";
    bool first = true;

    for (const auto& c : counters_) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << c->name() << "\":" << c->value();
    }

    for (const auto& g : gauges_) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << g->name() << "\":" << g->value();
    }

    oss << "}}";
    return oss.str();
}

}  // namespace quant::infra
