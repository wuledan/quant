// task_graph.h — DAG-based task graph for multi-step computations
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quant::scheduler {

using TaskId = uint64_t;

// ── Task execution result ──
enum class TaskStatus : uint8_t {
    kPending = 0,
    kRunning = 1,
    kCompleted = 2,
    kFailed = 3,
    kCancelled = 4,
};

// ── Task node ──
struct TaskNode {
    TaskId id;
    std::string name;
    std::function<void()> execute_fn;
    std::vector<TaskId> dependencies;
    std::atomic<TaskStatus> status{TaskStatus::kPending};
    std::string error_message;
    int64_t created_at{0};
    int64_t started_at{0};
    int64_t completed_at{0};

    TaskNode(TaskId id, std::string name, std::function<void()> fn)
        : id(id), name(std::move(name)), execute_fn(std::move(fn)) {}
};

// ── DAG validation result ──
struct GraphValidationResult {
    bool valid{true};
    std::string message;
    std::vector<TaskId> cycle_path;
};

// ── TaskGraph ──
class TaskGraph {
public:
    TaskGraph() = default;
    ~TaskGraph() = default;

    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    // Add a task node
    TaskId add_task(std::string name, std::function<void()> execute_fn);

    // Add a dependency: `task` depends on `depends_on`
    bool add_dependency(TaskId task, TaskId depends_on);

    // Get task by ID
    TaskNode* get_task(TaskId id);
    const TaskNode* get_task(TaskId id) const;

    // Validate the graph (cycle detection)
    GraphValidationResult validate() const;

    // Topological sort
    std::vector<TaskId> topological_sort() const;

    // Group tasks into parallel execution levels
    std::vector<std::vector<TaskId>> parallel_levels() const;

    // Get dependencies/dependents
    std::vector<TaskId> get_dependencies(TaskId id) const;
    std::vector<TaskId> get_dependents(TaskId id) const;

    // Clear all tasks
    void clear();

    // Number of tasks
    size_t size() const noexcept { return nodes_.size(); }

private:
    bool dfs_topo(TaskId id,
                   std::unordered_set<TaskId>& visited,
                   std::unordered_set<TaskId>& in_stack,
                   std::vector<TaskId>& order,
                   std::vector<TaskId>& cycle_path) const;

    std::unordered_map<TaskId, std::unique_ptr<TaskNode>> nodes_;
    std::unordered_map<TaskId, std::vector<TaskId>> deps_;        // task → its deps
    std::unordered_map<TaskId, std::vector<TaskId>> dependents_;  // task → its dependents
    TaskId next_id_{1};
};

}  // namespace quant::scheduler
