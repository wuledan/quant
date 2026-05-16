// task_graph.cc — TaskGraph implementation
#include "cpp/quant/scheduler/task_graph.h"

#include <algorithm>
#include <queue>

namespace quant::scheduler {

TaskId TaskGraph::add_task(std::string name, std::function<void()> execute_fn) {
    TaskId id = next_id_++;
    nodes_[id] = std::make_unique<TaskNode>(id, name, std::move(execute_fn));
    deps_[id] = {};
    dependents_[id] = {};
    return id;
}

bool TaskGraph::add_dependency(TaskId task, TaskId depends_on) {
    if (!nodes_.contains(task) || !nodes_.contains(depends_on)) return false;
    if (task == depends_on) return false;
    // Check if dependency already exists
    auto& tdeps = deps_[task];
    if (std::find(tdeps.begin(), tdeps.end(), depends_on) != tdeps.end()) return false;
    tdeps.push_back(depends_on);
    dependents_[depends_on].push_back(task);
    return true;
}

TaskNode* TaskGraph::get_task(TaskId id) {
    auto it = nodes_.find(id);
    return it != nodes_.end() ? it->second.get() : nullptr;
}

const TaskNode* TaskGraph::get_task(TaskId id) const {
    auto it = nodes_.find(id);
    return it != nodes_.end() ? it->second.get() : nullptr;
}

GraphValidationResult TaskGraph::validate() const {
    GraphValidationResult result;
    std::unordered_set<TaskId> visited, in_stack;
    std::vector<TaskId> order;
    for (const auto& [id, _] : nodes_) {
        if (!visited.contains(id)) {
            std::vector<TaskId> cycle_path;
            if (!dfs_topo(id, visited, in_stack, order, cycle_path)) {
                result.valid = false;
                result.message = "Cycle detected";
                result.cycle_path = std::move(cycle_path);
                return result;
            }
        }
    }
    result.message = "Graph is valid";
    return result;
}

std::vector<TaskId> TaskGraph::topological_sort() const {
    std::vector<TaskId> order;
    std::unordered_set<TaskId> visited, in_stack;
    std::vector<TaskId> cycle_path;
    for (const auto& [id, _] : nodes_) {
        if (!visited.contains(id)) {
            dfs_topo(id, visited, in_stack, order, cycle_path);
        }
    }
    return order;
}

std::vector<std::vector<TaskId>> TaskGraph::parallel_levels() const {
    std::unordered_map<TaskId, int> in_degree;
    for (const auto& [id, _] : nodes_) {
        in_degree[id] = static_cast<int>(deps_.at(id).size());
    }
    std::queue<TaskId> q;
    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) q.push(id);
    }
    std::vector<std::vector<TaskId>> levels;
    while (!q.empty()) {
        std::vector<TaskId> level;
        for (size_t sz = q.size(); sz > 0; --sz) {
            TaskId id = q.front(); q.pop();
            level.push_back(id);
            for (auto dep : dependents_.at(id)) {
                if (--in_degree[dep] == 0) q.push(dep);
            }
        }
        levels.push_back(std::move(level));
    }
    return levels;
}

std::vector<TaskId> TaskGraph::get_dependencies(TaskId id) const {
    auto it = deps_.find(id);
    return it != deps_.end() ? it->second : std::vector<TaskId>{};
}

std::vector<TaskId> TaskGraph::get_dependents(TaskId id) const {
    auto it = dependents_.find(id);
    return it != dependents_.end() ? it->second : std::vector<TaskId>{};
}

void TaskGraph::clear() {
    nodes_.clear();
    deps_.clear();
    dependents_.clear();
}

bool TaskGraph::dfs_topo(TaskId id,
                          std::unordered_set<TaskId>& visited,
                          std::unordered_set<TaskId>& in_stack,
                          std::vector<TaskId>& order,
                          std::vector<TaskId>& cycle_path) const {
    visited.insert(id);
    in_stack.insert(id);
    cycle_path.push_back(id);
    for (auto dep_id : deps_.at(id)) {
        if (!visited.contains(dep_id)) {
            if (!dfs_topo(dep_id, visited, in_stack, order, cycle_path)) return false;
        } else if (in_stack.contains(dep_id)) {
            cycle_path.push_back(dep_id);
            return false;
        }
    }
    in_stack.erase(id);
    cycle_path.pop_back();
    order.push_back(id);
    return true;
}

}  // namespace quant::scheduler
