// execution_dag.cc — ExecutionDAG implementation
#include "execution_dag.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <queue>

namespace quant::scheduler {

// ── Construction ──

TaskId ExecutionDAG::add_task(std::string name, TaskFunc func) {
    TaskId id = next_id_++;
    tasks_[id] = TaskNode{
        .id = id,
        .name = std::move(name),
        .func = std::move(func),
    };
    return id;
}

bool ExecutionDAG::add_dependency(TaskId task, TaskId depends_on) {
    auto tit = tasks_.find(task);
    auto dit = tasks_.find(depends_on);
    if (tit == tasks_.end() || dit == tasks_.end()) return false;
    if (task == depends_on) return false;

    auto& deps = tit->second.dependencies;
    if (std::find(deps.begin(), deps.end(), depends_on) != deps.end()) return false;

    deps.push_back(depends_on);
    dit->second.dependents.push_back(task);
    return true;
}

// ── Query ──

ValidationResult ExecutionDAG::validate() const {
    // Kahn's algorithm for cycle detection
    std::unordered_map<TaskId, int> in_degree;
    for (const auto& [id, node] : tasks_) {
        in_degree[id] = static_cast<int>(node.dependencies.size());
    }

    std::queue<TaskId> q;
    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) q.push(id);
    }

    size_t visited = 0;
    while (!q.empty()) {
        TaskId id = q.front();
        q.pop();
        ++visited;
        for (auto dep : tasks_.at(id).dependents) {
            if (--in_degree[dep] == 0) q.push(dep);
        }
    }

    if (visited == tasks_.size()) {
        return {.ok = true, .error = ""};
    }
    return {.ok = false, .error = "Cycle detected in DAG"};
}

std::vector<std::vector<TaskId>> ExecutionDAG::parallel_levels() const {
    std::unordered_map<TaskId, int> in_degree;
    for (const auto& [id, node] : tasks_) {
        in_degree[id] = static_cast<int>(node.dependencies.size());
    }

    std::queue<TaskId> q;
    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) q.push(id);
    }

    std::vector<std::vector<TaskId>> levels;
    while (!q.empty()) {
        std::vector<TaskId> level;
        for (size_t sz = q.size(); sz > 0; --sz) {
            TaskId id = q.front();
            q.pop();
            level.push_back(id);
            for (auto dep : tasks_.at(id).dependents) {
                if (--in_degree[dep] == 0) q.push(dep);
            }
        }
        levels.push_back(std::move(level));
    }

    return levels;
}

// ── Execution ──

folly::coro::Task<ExecutionResult>
ExecutionDAG::co_execute(infra::WorkStealingExecutor& executor) {
    auto start_ts = std::chrono::steady_clock::now();

    // 1. Validate
    auto vr = validate();
    if (!vr.ok) {
        auto elapsed = std::chrono::steady_clock::now() - start_ts;
        co_return ExecutionResult{
            .success = false,
            .error = std::move(vr.error),
            .total_time_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                    .count()),
        };
    }

    // 2. Get levels
    auto levels = parallel_levels();

    // 3. Execute level by level
    std::atomic<bool> any_failed{false};
    std::mutex error_mutex;
    std::string combined_error;

    auto& stats = infra::StatRegistry::instance();

    for (size_t i = 0; i < levels.size(); ++i) {
        auto level_start = std::chrono::steady_clock::now();

        // Execute tasks in this level in parallel via executor
        // Use co_withExecutor to bind each sub-task to the executor,
        // then collectAll for parallel execution.
        // Avoid vector<Task> (bad_alloc from move semantics) —
        // instead submit tasks to executor and wait via baton/countdown.
        std::atomic<size_t> completed{0};
        std::atomic<size_t> failed{0};
        size_t level_size = levels[i].size();
        std::promise<void> level_promise;
        auto level_future = level_promise.get_future();

        for (auto id : levels[i]) {
            auto& node = tasks_[id];
            executor.add([&node, &any_failed, &error_mutex, &combined_error,
                          &completed, &failed, level_size, &level_promise]() {
                try {
                    // node.func() returns folly::coro::Task<void>
                    // For now, execute synchronously within this worker
                    // (the func itself may contain co_await points)
                    // We use blockingWait here as a bridge
                    infra::blockingWait(node.func());
                    infra::StatRegistry::instance().increment(
                        "dag.tasks_executed");
                } catch (const std::exception& e) {
                    any_failed.store(true, std::memory_order_relaxed);
                    failed.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard lk(error_mutex);
                    if (!combined_error.empty()) combined_error += "; ";
                    combined_error += node.name + ": " + e.what();
                }

                if (completed.fetch_add(1, std::memory_order_acq_rel) + 1
                    == level_size) {
                    level_promise.set_value();
                }
            });
        }

        level_future.wait();

        auto level_end = std::chrono::steady_clock::now();
        auto level_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                level_end - level_start)
                .count();
        stats.observe_timer(
            "dag.layer_" + std::to_string(i) + "_latency_ms",
            level_ms / 1000.0);
    }

    auto total_end = std::chrono::steady_clock::now();
    co_return ExecutionResult{
        .success = !any_failed.load(),
        .error = combined_error,
        .total_time_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                total_end - start_ts)
                .count()),
    };
}

ExecutionResult ExecutionDAG::execute(infra::WorkStealingExecutor& executor) {
    return infra::blockingWait(co_execute(executor));
}

}  // namespace quant::scheduler
