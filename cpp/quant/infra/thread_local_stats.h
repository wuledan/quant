// thread_local_stats.h — Thread-local statistics with cross-thread aggregation
//
// Design:
//   - Each thread accumulates counters/gauges in its own thread_local
//     LocalHolder, avoiding contention on shared atomic counters
//   - LocalHolder auto-registers on construction, auto-unregisters
//     on thread exit (via destructor)
//   - aggregate() iterates all registered holders to produce a global snapshot
//   - reset() clears all thread-local data
//
// Usage:
//   auto& registry = StatRegistry::instance();
//   registry.increment("orders_submitted");
//   registry.set_gauge("portfolio_value", 1_500_000.0);
//   registry.observe_timer("factor_compute", 0.023);
//   auto snap = registry.aggregate();  // sums across all threads
#pragma once

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace quant::infra {

class StatRegistry {
public:
    // ── Per-thread accumulator (auto-registers/unregisters) ──
    class LocalHolder {
    public:
        explicit LocalHolder(StatRegistry* registry) : registry_(registry) {
            registry_->register_holder(this);
        }

        ~LocalHolder() {
            if (registry_) {
                registry_->unregister_holder(this);
            }
        }

        LocalHolder(const LocalHolder&) = delete;
        LocalHolder& operator=(const LocalHolder&) = delete;

        void increment(std::string_view key, int64_t delta = 1) {
            counters_[std::string(key)] += delta;
        }

        void set_gauge(std::string_view key, double value) {
            gauges_[std::string(key)] = value;
        }

        void observe_timer(std::string_view key, double seconds) {
            auto& entry = timers_[std::string(key)];
            entry.first += seconds;
            entry.second += 1;
        }

        // Accessed by aggregate() under registry mutex
        const auto& counters() const { return counters_; }
        const auto& gauges() const { return gauges_; }
        const auto& timers() const { return timers_; }

        void reset() {
            counters_.clear();
            gauges_.clear();
            timers_.clear();
        }

    private:
        StatRegistry* registry_;
        std::unordered_map<std::string, int64_t> counters_;
        std::unordered_map<std::string, double> gauges_;
        // timer name -> (total_seconds, observation_count)
        std::unordered_map<std::string, std::pair<double, size_t>> timers_;
    };

    // ── Aggregated snapshot ──
    struct Snapshot {
        std::unordered_map<std::string, int64_t> counters;
        std::unordered_map<std::string, double> gauges;
        // timer name -> (total_seconds, observation_count, avg_seconds)
        std::unordered_map<std::string, std::tuple<double, size_t, double>> timers;
        size_t thread_count = 0;
    };

    // ── Singleton ──
    static StatRegistry& instance() {
        static StatRegistry registry;
        return registry;
    }

    // ── Thread-local accumulator ──
    LocalHolder& local() {
        thread_local LocalHolder holder(this);
        return holder;
    }

    // ── Convenience methods ──
    void increment(std::string_view key, int64_t delta = 1) {
        local().increment(key, delta);
    }

    void set_gauge(std::string_view key, double value) {
        local().set_gauge(key, value);
    }

    void observe_timer(std::string_view key, double seconds) {
        local().observe_timer(key, seconds);
    }

    // ── Aggregate all thread-local data ──
    Snapshot aggregate() const {
        Snapshot snap;
        std::lock_guard lock(mutex_);

        snap.thread_count = holders_.size();

        for (const auto* holder : holders_) {
            for (const auto& [k, v] : holder->counters()) {
                snap.counters[k] += v;
            }
            // For gauges, take the latest non-zero value across threads
            for (const auto& [k, v] : holder->gauges()) {
                snap.gauges[k] = v;  // Last writer wins
            }
            for (const auto& [k, v] : holder->timers()) {
                auto& [total, count, avg] = snap.timers[k];
                total += v.first;
                count += v.second;
            }
        }

        // Compute averages
        for (auto& [k, tup] : snap.timers) {
            auto& [total, count, avg] = tup;
            avg = count > 0 ? total / static_cast<double>(count) : 0.0;
        }

        return snap;
    }

    // ── Reset all thread-local data ──
    void reset() {
        std::lock_guard lock(mutex_);
        for (auto* holder : holders_) {
            holder->reset();
        }
    }

    // ── Number of registered threads ──
    size_t thread_count() const {
        std::lock_guard lock(mutex_);
        return holders_.size();
    }

private:
    StatRegistry() = default;

    void register_holder(LocalHolder* holder) {
        std::lock_guard lock(mutex_);
        holders_.push_back(holder);
    }

    void unregister_holder(LocalHolder* holder) {
        std::lock_guard lock(mutex_);
        auto it = std::find(holders_.begin(), holders_.end(), holder);
        if (it != holders_.end()) {
            holders_.erase(it);
        }
    }

    mutable std::mutex mutex_;
    std::vector<LocalHolder*> holders_;
};

}  // namespace quant::infra
