// scheduler_service.h — Top-level scheduler facade
#pragma once

#include <memory>
#include <vector>

#include "cpp/quant/scheduler/cron_scheduler.h"
#include "cpp/quant/scheduler/task_graph.h"
#include "cpp/quant/scheduler/wave_scheduler.h"

namespace quant::scheduler {

// ── SchedulerService: unified scheduling facade ──
class SchedulerService {
public:
    SchedulerService();
    ~SchedulerService();

    SchedulerService(const SchedulerService&) = delete;
    SchedulerService& operator=(const SchedulerService&) = delete;

    // Wave scheduling
    TaskGraph& graph() noexcept { return *graph_; }
    WaveScheduler& wave() noexcept { return *wave_; }

    WaveExecutionResult run_graph();

    // Cron scheduling
    CronScheduler& cron() noexcept { return *cron_; }

    uint64_t schedule_cron(std::string name, std::string cron_expr,
                            std::function<void()> action);

    // Global control
    void start();
    void stop();

private:
    std::unique_ptr<TaskGraph> graph_;
    std::unique_ptr<WaveScheduler> wave_;
    std::unique_ptr<CronScheduler> cron_;
    bool started_{false};
};

}  // namespace quant::scheduler
