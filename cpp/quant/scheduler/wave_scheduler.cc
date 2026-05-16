// wave_scheduler.cc — WaveScheduler implementation
#include "cpp/quant/scheduler/wave_scheduler.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace quant::scheduler {

WaveScheduler::WaveScheduler(WaveSchedulerConfig config)
    : config_(std::move(config)) {}

WaveScheduler::~WaveScheduler() = default;

WaveExecutionResult WaveScheduler::execute(TaskGraph& graph) {
    auto validation = graph.validate();
    if (!validation.valid) {
        WaveExecutionResult r;
        r.success = false;
        r.error_message = "Graph validation failed: " + validation.message;
        return r;
    }

    auto levels = graph.parallel_levels();
    WaveExecutionResult result;
    result.total_tasks = graph.size();

    auto start = std::chrono::high_resolution_clock::now();

    for (auto& level : levels) {
        std::vector<std::thread> threads;
        size_t concurrency = std::min(config_.max_concurrency, level.size());

        for (size_t i = 0; i < level.size(); i += concurrency) {
            threads.clear();
            size_t end = std::min(i + concurrency, level.size());

            for (size_t j = i; j < end; ++j) {
                TaskId id = level[j];
                threads.emplace_back([&graph, id, &result]() {
                    auto* task = graph.get_task(id);
                    if (!task) return;

                    TaskStatus expected = TaskStatus::kPending;
                    if (!task->status.compare_exchange_strong(
                            expected, TaskStatus::kRunning)) return;

                    auto tstart = std::chrono::high_resolution_clock::now();
                    task->started_at = std::chrono::duration_cast<
                        std::chrono::milliseconds>(tstart.time_since_epoch()).count();

                    try {
                        task->execute_fn();
                        task->status.store(TaskStatus::kCompleted);
                    } catch (const std::exception& e) {
                        task->status.store(TaskStatus::kFailed);
                        task->error_message = e.what();
                    }

                    auto tend = std::chrono::high_resolution_clock::now();
                    task->completed_at = std::chrono::duration_cast<
                        std::chrono::milliseconds>(tend.time_since_epoch()).count();
                    auto duration = std::chrono::duration_cast<
                        std::chrono::nanoseconds>(tend - tstart).count();

                    if (task->status.load() == TaskStatus::kCompleted) {
                        result.completed_tasks++;
                    } else {
                        result.failed_tasks++;
                    }
                    result.task_timings.emplace_back(id, duration);
                });
            }

            for (auto& t : threads) {
                if (t.joinable()) t.join();
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.total_duration_ns = std::chrono::duration_cast<
        std::chrono::nanoseconds>(end - start).count();
    result.success = (result.failed_tasks == 0);

    return result;
}

WaveExecutionResult WaveScheduler::execute_tasks(
    TaskGraph& graph, const std::vector<TaskId>& task_ids) {
    // Traverse to collect all transitive dependencies
    std::unordered_set<TaskId> all_ids(task_ids.begin(), task_ids.end());
    std::vector<TaskId> stack(task_ids.begin(), task_ids.end());
    while (!stack.empty()) {
        TaskId id = stack.back(); stack.pop_back();
        for (auto dep : graph.get_dependencies(id)) {
            if (!all_ids.contains(dep)) {
                all_ids.insert(dep);
                stack.push_back(dep);
            }
        }
    }

    // Build filtered graph
    TaskGraph filtered;
    std::unordered_map<TaskId, TaskId> id_map;
    for (auto id : all_ids) {
        auto* task = graph.get_task(id);
        if (!task) continue;
        auto new_id = filtered.add_task(task->name, task->execute_fn);
        id_map[id] = new_id;
    }

    for (auto id : all_ids) {
        for (auto dep : graph.get_dependencies(id)) {
            if (id_map.contains(dep) && id_map.contains(id)) {
                filtered.add_dependency(id_map[id], id_map[dep]);
            }
        }
    }

    return execute(filtered);
}

}  // namespace quant::scheduler
