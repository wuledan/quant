// execution_dag.h — Unified DAG orchestration layer with coroutine-based execution
#pragma once

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/infra/thread_local_stats.h"
#include "cpp/quant/infra/work_stealing_executor.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace quant::scheduler {

using TaskId = uint32_t;
using TaskFunc = std::function<folly::coro::Task<void>()>;

struct ValidationResult {
    bool ok;
    std::string error;
};

struct ExecutionResult {
    bool success;
    std::string error;
    uint64_t total_time_ms;
};

// ── ExecutionDAG ──
//
// A unified DAG that owns TaskNodes and executes them in parallel levels.
// Each level runs its tasks concurrently via folly::coro::collectAllRange.
//
class ExecutionDAG {
public:
    ExecutionDAG() = default;
    ~ExecutionDAG() = default;

    ExecutionDAG(const ExecutionDAG&) = delete;
    ExecutionDAG& operator=(const ExecutionDAG&) = delete;

    // ── Construction ──
    TaskId add_task(std::string name, TaskFunc func);
    bool add_dependency(TaskId task, TaskId depends_on);

    // ── Query ──
    ValidationResult validate() const;
    std::vector<std::vector<TaskId>> parallel_levels() const;
    size_t size() const noexcept { return tasks_.size(); }

    // ── Execution ──
    folly::coro::Task<ExecutionResult> co_execute(infra::WorkStealingExecutor& executor);
    ExecutionResult execute(infra::WorkStealingExecutor& executor);

private:
    struct TaskNode {
        TaskId id;
        std::string name;
        TaskFunc func;
        std::vector<TaskId> dependencies;
        std::vector<TaskId> dependents;
    };

    std::unordered_map<TaskId, TaskNode> tasks_;
    TaskId next_id_ = 0;
};

}  // namespace quant::scheduler
