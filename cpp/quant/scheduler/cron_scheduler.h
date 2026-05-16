// cron_scheduler.h — Cron-based time-triggered scheduling
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace quant::scheduler {

struct CronJob {
    uint64_t id;
    std::string name;
    std::string cron_expression;
    std::function<void()> action;
    bool enabled{true};
    int64_t last_run{0};
    int64_t next_run{0};
};

struct CronSchedulerConfig {
    size_t worker_pool_size = 2;
    int64_t tick_interval_ms = 1000;
};

class CronScheduler {
public:
    explicit CronScheduler(CronSchedulerConfig config = {});
    ~CronScheduler();

    CronScheduler(const CronScheduler&) = delete;
    CronScheduler& operator=(const CronScheduler&) = delete;

    uint64_t add_job(std::string name, std::string cron_expression, std::function<void()> action);
    bool remove_job(uint64_t job_id);
    void enable_job(uint64_t job_id, bool enabled);
    void start();
    void stop();

    static int64_t next_match(const std::string& cron_expr, int64_t after_timestamp);
    static bool matches(const std::string& cron_expr, int64_t timestamp);

    bool is_running() const noexcept { return running_.load(); }
    std::vector<CronJob> list_jobs() const;

private:
    void tick();

    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> scheduler_thread_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, CronJob> jobs_;
    uint64_t next_job_id_{1};
    CronSchedulerConfig config_;
};

}  // namespace quant::scheduler
