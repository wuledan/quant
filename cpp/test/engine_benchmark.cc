// engine_benchmark.cc — Engine performance benchmarks
// Measures DAG scheduling, executor throughput, and coroutine overhead

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "coroutine.h"
#include "work_stealing_executor.h"
#include "task_graph.h"
#include "wave_scheduler.h"

namespace quant { namespace benchmark {

using namespace quant::infra;
using Clock = std::chrono::high_resolution_clock;

struct BenchStats {
    double mean_us{0};
    double median_us{0};
    double min_us{0};
    double max_us{0};
    double stddev_us{0};
    double throughput_ops_per_sec{0};
};

static BenchStats compute_stats(const std::vector<double>& samples, size_t ops_per_sample = 1) {
    BenchStats s;
    if (samples.empty()) return s;
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    s.min_us = sorted.front();
    s.max_us = sorted.back();
    s.median_us = sorted[sorted.size() / 2];
    double sum = 0;
    for (auto v : sorted) sum += v;
    s.mean_us = sum / sorted.size();
    double sq_sum = 0;
    for (auto v : sorted) sq_sum += (v - s.mean_us) * (v - s.mean_us);
    s.stddev_us = std::sqrt(sq_sum / sorted.size());
    double total_sec = sum / 1e6;
    s.throughput_ops_per_sec = (sorted.size() * ops_per_sample) / total_sec;
    return s;
}

static void print_stats(const std::string& name, const BenchStats& s, const std::string& unit = "us") {
    std::cout << "  " << name << ":\n";
    std::cout << "    mean: " << s.mean_us << " " << unit
              << "  median: " << s.median_us << " " << unit
              << "  min: " << s.min_us << " " << unit
              << "  max: " << s.max_us << " " << unit << "\n";
    std::cout << "    stddev: " << s.stddev_us << " " << unit
              << "  throughput: " << s.throughput_ops_per_sec << " ops/s\n";
}

// ── 1. WorkStealingExecutor throughput ──

TEST(EngineBenchmark, ExecutorTaskThroughput) {
    constexpr size_t kWorkers = 4;
    constexpr size_t kTasks = 100000;

    WorkStealingExecutor ex(kWorkers);
    ex.start();

    std::vector<double> samples;
    samples.reserve(10);

    for (int trial = 0; trial < 10; ++trial) {
        std::atomic<size_t> counter{0};
        std::promise<void> done;
        auto f = done.get_future();

        auto start = Clock::now();
        for (size_t i = 0; i < kTasks; ++i) {
            ex.add([&counter, &done, total = kTasks]() {
                counter.fetch_add(1, std::memory_order_relaxed);
                if (counter.load(std::memory_order_acquire) == total) {
                    done.set_value();
                }
            });
        }
        f.wait();
        auto end = Clock::now();
        double us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        samples.push_back(us);
    }

    ex.stop();

    auto s = compute_stats(samples, kTasks);
    std::cout << "\n[Executor] " << kTasks << " tasks, " << kWorkers << " workers:\n";
    print_stats("submit+execute", s);
}

// ── 2. WaveScheduler DAG throughput ──

TEST(EngineBenchmark, DagSmall) {
    WorkStealingExecutor ex(4);
    ex.start();

    std::vector<double> samples;
    for (int t = 0; t < 10; ++t) {
        quant::scheduler::WaveScheduler scheduler;
        quant::scheduler::TaskGraph graph;

        // 100 tasks, 10 levels (chain)
        std::vector<quant::scheduler::TaskId> prev;
        for (int l = 0; l < 10; ++l) {
            std::vector<quant::scheduler::TaskId> curr;
            for (int j = 0; j < 10; ++j) {
                auto id = graph.add_task("t", [](){});
                curr.push_back(id);
                if (l > 0) {
                    graph.add_dependency(id, prev[j % prev.size()]);
                }
            }
            prev = std::move(curr);
        }

        auto start = Clock::now();
        auto r = blockingWait(scheduler.execute_async(graph, ex));
        auto end = Clock::now();
        double us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        samples.push_back(us);
        EXPECT_TRUE(r.success);
    }
    ex.stop();

    auto s = compute_stats(samples, 100);
    std::cout << "\n[DAG Small] 100 tasks, 10 levels, 4 workers:\n";
    print_stats("execute", s);
}

TEST(EngineBenchmark, DagMedium) {
    WorkStealingExecutor ex(4);
    ex.start();

    std::vector<double> samples;
    for (int t = 0; t < 5; ++t) {
        quant::scheduler::WaveScheduler scheduler;
        quant::scheduler::TaskGraph graph;

        std::vector<quant::scheduler::TaskId> prev;
        for (int l = 0; l < 10; ++l) {
            std::vector<quant::scheduler::TaskId> curr;
            for (int j = 0; j < 100; ++j) {
                auto id = graph.add_task("t", [](){});
                curr.push_back(id);
                if (l > 0) {
                    graph.add_dependency(id, prev[j % prev.size()]);
                }
            }
            prev = std::move(curr);
        }

        auto start = Clock::now();
        auto r = blockingWait(scheduler.execute_async(graph, ex));
        auto end = Clock::now();
        double us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        samples.push_back(us);
        EXPECT_TRUE(r.success);
    }
    ex.stop();

    auto s = compute_stats(samples, 1000);
    std::cout << "\n[DAG Medium] 1000 tasks, 10 levels, 4 workers:\n";
    print_stats("execute", s);
}

TEST(EngineBenchmark, DagWide) {
    WorkStealingExecutor ex(8);
    ex.start();

    std::vector<double> samples;
    for (int t = 0; t < 5; ++t) {
        quant::scheduler::WaveScheduler scheduler;
        quant::scheduler::TaskGraph graph;

        std::vector<quant::scheduler::TaskId> prev;
        for (int l = 0; l < 3; ++l) {
            std::vector<quant::scheduler::TaskId> curr;
            for (int j = 0; j < 1000; ++j) {
                auto id = graph.add_task("t", [](){});
                curr.push_back(id);
                if (l > 0) {
                    graph.add_dependency(id, prev[j % prev.size()]);
                }
            }
            prev = std::move(curr);
        }

        auto start = Clock::now();
        auto r = blockingWait(scheduler.execute_async(graph, ex));
        auto end = Clock::now();
        double us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        samples.push_back(us);
        EXPECT_TRUE(r.success);
    }
    ex.stop();

    auto s = compute_stats(samples, 3000);
    std::cout << "\n[DAG Wide] 3000 tasks, 3 levels, 8 workers:\n";
    print_stats("execute", s);
}

// ── 3. Coroutine overhead ──

static CoTask<void> empty_coro() { co_return; }

static CoTask<void> chain_coro(int depth) {
    if (depth > 0) {
        co_await chain_coro(depth - 1);
    }
    co_return;
}

TEST(EngineBenchmark, CoroutineCreationOverhead) {
    std::vector<double> samples;
    for (int t = 0; t < 10; ++t) {
        auto start = Clock::now();
        for (int i = 0; i < 10000; ++i) {
            blockingWait(empty_coro());
        }
        auto end = Clock::now();
        double us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        samples.push_back(us);
    }

    auto s = compute_stats(samples, 10000);
    std::cout << "\n[Coroutine] 10000 empty coroutine create+execute:\n";
    print_stats("blockingWait", s);
}

TEST(EngineBenchmark, CoroutineChainOverhead) {
    constexpr int kDepth = 64;
    std::vector<double> samples;
    for (int t = 0; t < 10; ++t) {
        auto start = Clock::now();
        for (int i = 0; i < 1000; ++i) {
            blockingWait(chain_coro(kDepth));
        }
        auto end = Clock::now();
        double us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        samples.push_back(us);
    }

    auto s = compute_stats(samples, 1000);
    std::cout << "\n[Coroutine Chain] 1000 chains of depth " << kDepth << ":\n";
    print_stats("blockingWait", s);
    std::cout << "    per co_await: " << (s.mean_us * 1000 / 1000 / kDepth) << " ns\n";
}

}}  // namespace quant::benchmark
