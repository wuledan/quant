// wave_scheduler.h — Wave-based parallel task scheduler
// Executes tasks in parallel levels respecting DAG dependencies
#pragma once

#include <memory>
#include <vector>

#include "cpp/quant/scheduler/task_graph.h"

namespace quant::scheduler {

// ── Wave scheduler configuration ──
struct WaveSchedulerConfig {
    size_t max_concurrency = 4;  // max threads for parallel execution
    bool   enable_profiling = false;
};

// ── Schedule execution result ──
struct WaveExecutionResult {
    bool success{true};
    std::string error_message;
    size_t total_tasks{0};
    size_t completed_tasks{0};
    size_t failed_tasks{0};
    int64_t total_duration_ns{0};
    std::vector<std::pair<TaskId, int64_t>> task_timings;
};

// ── WaveScheduler: executes tasks in parallel waves ──
class WaveScheduler {
public:
    explicit WaveScheduler(WaveSchedulerConfig config = {});
    ~WaveScheduler();

    WaveScheduler(const WaveScheduler&) = delete;
    WaveScheduler& operator=(const WaveScheduler&) = delete;

    WaveExecutionResult execute(TaskGraph& graph);
    WaveExecutionResult execute_tasks(TaskGraph& graph,
                                       const std::vector<TaskId>& task_ids);

    const WaveSchedulerConfig& config() const noexcept { return config_; }

private:
    WaveSchedulerConfig config_;
};

}  // namespace quant::scheduler
