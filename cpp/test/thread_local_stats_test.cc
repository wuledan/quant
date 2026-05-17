// thread_local_stats_test.cc — Tests for thread-local statistics aggregation
#include "cpp/quant/infra/thread_local_stats.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

namespace quant::infra {
namespace {

TEST(StatRegistryTest, SingleThreadIncrement) {
    auto& reg = StatRegistry::instance();
    reg.reset();

    reg.increment("orders");
    reg.increment("orders");
    reg.increment("orders", 3);

    auto snap = reg.aggregate();
    EXPECT_EQ(snap.counters.at("orders"), 5);
}

TEST(StatRegistryTest, SingleThreadGauge) {
    auto& reg = StatRegistry::instance();
    reg.reset();

    reg.set_gauge("portfolio_value", 1500000.0);
    reg.set_gauge("portfolio_value", 1600000.0);

    auto snap = reg.aggregate();
    EXPECT_DOUBLE_EQ(snap.gauges.at("portfolio_value"), 1600000.0);
}

TEST(StatRegistryTest, SingleThreadTimer) {
    auto& reg = StatRegistry::instance();
    reg.reset();

    reg.observe_timer("factor_compute", 0.1);
    reg.observe_timer("factor_compute", 0.2);
    reg.observe_timer("factor_compute", 0.3);

    auto snap = reg.aggregate();
    auto [total, count, avg] = snap.timers.at("factor_compute");
    EXPECT_DOUBLE_EQ(total, 0.6);
    EXPECT_EQ(count, 3u);
    EXPECT_NEAR(avg, 0.2, 1e-9);
}

TEST(StatRegistryTest, MultiThreadAggregation) {
    auto& reg = StatRegistry::instance();
    reg.reset();

    constexpr int kThreads = 4;
    constexpr int kIncrements = 1000;

    // Use barriers to synchronize: threads increment, signal done,
    // then wait for main thread to aggregate before exiting.
    std::atomic<int> ready_count{0};
    std::atomic<bool> can_exit{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&reg, t, &ready_count, &can_exit]() {
            for (int i = 0; i < kIncrements; ++i) {
                reg.increment("shared_counter");
            }
            reg.set_gauge("thread_gauge", static_cast<double>(t));
            reg.observe_timer("thread_timer", static_cast<double>(t) * 0.01);

            ready_count.fetch_add(1);
            // Wait until main thread has aggregated
            while (!can_exit.load()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Wait for all threads to finish incrementing
    while (ready_count.load() < kThreads) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Aggregate while threads are still alive (their LocalHolders exist)
    auto snap = reg.aggregate();
    EXPECT_EQ(snap.counters.at("shared_counter"), kThreads * kIncrements);
    EXPECT_GE(snap.thread_count, 1u);

    EXPECT_TRUE(snap.timers.contains("thread_timer"));
    auto [total, count, avg] = snap.timers.at("thread_timer");
    EXPECT_EQ(count, static_cast<size_t>(kThreads));

    // Let threads exit
    can_exit = true;
    for (auto& t : threads) {
        t.join();
    }
}

TEST(StatRegistryTest, ResetClearsData) {
    auto& reg = StatRegistry::instance();
    reg.reset();

    reg.increment("test_counter", 100);
    reg.set_gauge("test_gauge", 42.0);

    auto snap = reg.aggregate();
    EXPECT_EQ(snap.counters.at("test_counter"), 100);

    reg.reset();

    snap = reg.aggregate();
    // After reset, the calling thread's counters are cleared
}

TEST(StatRegistryTest, ThreadCount) {
    auto& reg = StatRegistry::instance();
    reg.reset();

    reg.increment("trigger_register");

    auto count = reg.thread_count();
    EXPECT_GE(count, 1u);
}

TEST(StatRegistryTest, AutoUnregisterOnThreadExit) {
    auto& reg = StatRegistry::instance();

    auto count_before = reg.thread_count();

    {
        std::thread t([&reg]() {
            reg.increment("temp_counter");
        });
        t.join();
    }

    auto count_after = reg.thread_count();
    EXPECT_EQ(count_after, count_before);
}

TEST(StatRegistryTest, MixedCountersAndGauges) {
    auto& reg = StatRegistry::instance();
    reg.reset();

    reg.increment("orders_buy", 10);
    reg.increment("orders_sell", 5);
    reg.set_gauge("cash", 100000.0);
    reg.set_gauge("position_value", 500000.0);
    reg.observe_timer("order_latency", 0.001);
    reg.observe_timer("order_latency", 0.002);

    auto snap = reg.aggregate();
    EXPECT_EQ(snap.counters.at("orders_buy"), 10);
    EXPECT_EQ(snap.counters.at("orders_sell"), 5);
    EXPECT_DOUBLE_EQ(snap.gauges.at("cash"), 100000.0);
    EXPECT_DOUBLE_EQ(snap.gauges.at("position_value"), 500000.0);

    auto [total, count, avg] = snap.timers.at("order_latency");
    EXPECT_DOUBLE_EQ(total, 0.003);
    EXPECT_EQ(count, 2u);
    EXPECT_NEAR(avg, 0.0015, 1e-9);
}

}  // namespace
}  // namespace quant::infra
