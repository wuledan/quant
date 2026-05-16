// metrics_test.cc — Tests for Counter, Gauge, Histogram, MetricsRegistry
#include "cpp/quant/infra/logging/metrics.h"
#include <gtest/gtest.h>
#include <cmath>
#include <thread>
#include <vector>

namespace quant::infra {
namespace {

// ========================================================================
// Counter tests
// ========================================================================

TEST(CounterTest, InitialValue) {
    Counter c("test_counter");
    EXPECT_EQ(c.value(), 0);
    EXPECT_EQ(c.name(), "test_counter");
}

TEST(CounterTest, Increment) {
    Counter c("test");
    c.increment();
    EXPECT_EQ(c.value(), 1);
    c.increment(5);
    EXPECT_EQ(c.value(), 6);
}

TEST(CounterTest, Reset) {
    Counter c("test");
    c.increment(10);
    EXPECT_EQ(c.value(), 10);
    c.reset();
    EXPECT_EQ(c.value(), 0);
}

// ========================================================================
// Gauge tests
// ========================================================================

TEST(GaugeTest, InitialValue) {
    Gauge g("test_gauge");
    EXPECT_DOUBLE_EQ(g.value(), 0.0);
}

TEST(GaugeTest, Set) {
    Gauge g("test");
    g.set(42.5);
    EXPECT_DOUBLE_EQ(g.value(), 42.5);
}

TEST(GaugeTest, IncrementDecrement) {
    Gauge g("test");
    g.set(10.0);
    g.increment(5.0);
    EXPECT_DOUBLE_EQ(g.value(), 15.0);
    g.decrement(3.0);
    EXPECT_DOUBLE_EQ(g.value(), 12.0);
}

// ========================================================================
// Histogram tests
// ========================================================================

TEST(HistogramTest, EmptyHistogram) {
    Histogram h("test_hist", {1.0, 5.0, 10.0});
    EXPECT_EQ(h.count(), 0);
    EXPECT_DOUBLE_EQ(h.sum(), 0.0);
    EXPECT_DOUBLE_EQ(h.min(), 0.0);
    EXPECT_DOUBLE_EQ(h.max(), 0.0);
    EXPECT_DOUBLE_EQ(h.mean(), 0.0);
}

TEST(HistogramTest, SingleObservation) {
    Histogram h("test_hist", {1.0, 5.0, 10.0});
    h.observe(3.0);

    EXPECT_EQ(h.count(), 1);
    EXPECT_DOUBLE_EQ(h.sum(), 3.0);
    EXPECT_DOUBLE_EQ(h.min(), 3.0);
    EXPECT_DOUBLE_EQ(h.max(), 3.0);
    EXPECT_DOUBLE_EQ(h.mean(), 3.0);
}

TEST(HistogramTest, MultipleObservations) {
    Histogram h("test_hist", {1.0, 5.0, 10.0});
    h.observe(0.5);   // bucket 0 (<=1.0)
    h.observe(3.0);   // bucket 1 (<=5.0)
    h.observe(7.0);   // bucket 2 (<=10.0)
    h.observe(20.0);  // bucket 3 (+Inf)

    EXPECT_EQ(h.count(), 4);
    EXPECT_DOUBLE_EQ(h.sum(), 30.5);
    EXPECT_DOUBLE_EQ(h.min(), 0.5);
    EXPECT_DOUBLE_EQ(h.max(), 20.0);
    EXPECT_DOUBLE_EQ(h.mean(), 30.5 / 4.0);

    auto counts = h.bucket_counts();
    ASSERT_EQ(counts.size(), 4);
    EXPECT_EQ(counts[0], 1);  // 0.5
    EXPECT_EQ(counts[1], 1);  // 3.0
    EXPECT_EQ(counts[2], 1);  // 7.0
    EXPECT_EQ(counts[3], 1);  // 20.0
}

TEST(HistogramTest, ExactBoundary) {
    Histogram h("test", {1.0, 5.0});
    h.observe(1.0);   // should go to bucket 0 (value <= 1.0)
    h.observe(5.0);   // should go to bucket 1 (value <= 5.0)

    auto counts = h.bucket_counts();
    EXPECT_EQ(counts[0], 1);
    EXPECT_EQ(counts[1], 1);
}

TEST(HistogramTest, Percentile) {
    Histogram h("test", {1.0, 5.0, 10.0});
    for (int i = 0; i < 100; ++i) {
        h.observe(i % 10);
    }
    // 100 values: 10x each of 0..9
    // P50 should be around 5 (since values span 0-9)
    double p50 = h.percentile(50.0);
    EXPECT_GE(p50, 0.0);
    EXPECT_LE(p50, 10.0);
}

// ========================================================================
// MetricsRegistry tests
// ========================================================================

TEST(MetricsRegistryTest, Singleton) {
    auto& r1 = MetricsRegistry::instance();
    auto& r2 = MetricsRegistry::instance();
    EXPECT_EQ(&r1, &r2);
}

TEST(MetricsRegistryTest, CreateCounter) {
    auto& reg = MetricsRegistry::instance();
    auto& c = reg.counter("requests_total", "Total requests");
    EXPECT_EQ(c.name(), "requests_total");
}

TEST(MetricsRegistryTest, CreateGauge) {
    auto& reg = MetricsRegistry::instance();
    auto& g = reg.gauge("temperature", "Current temperature");
    EXPECT_EQ(g.name(), "temperature");
}

TEST(MetricsRegistryTest, CreateHistogram) {
    auto& reg = MetricsRegistry::instance();
    auto& h = reg.histogram("latency_ms", {1.0, 5.0, 10.0});
    EXPECT_EQ(h.name(), "latency_ms");
}

TEST(MetricsRegistryTest, PrometheusOutput) {
    auto& reg = MetricsRegistry::instance();
    // These are created cumulatively; just verify output is non-empty
    std::string prom = reg.to_prometheus();
    EXPECT_FALSE(prom.empty());
    EXPECT_NE(prom.find("# HELP"), std::string::npos);
    EXPECT_NE(prom.find("# TYPE"), std::string::npos);
}

TEST(MetricsRegistryTest, JsonOutput) {
    auto& reg = MetricsRegistry::instance();
    std::string json = reg.to_json();
    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("metrics"), std::string::npos);
}

TEST(MetricsRegistryTest, ConcurrentAccess) {
    auto& reg = MetricsRegistry::instance();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 10; ++i) {
                reg.counter("concurrent_counter", "");
                reg.gauge("concurrent_gauge", "");
            }
        });
    }
    for (auto& t : threads) t.join();
}

}  // namespace
}  // namespace quant::infra
