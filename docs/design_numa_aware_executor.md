# NUMA 感知 WorkStealingExecutor 设计

> 日期: 2026-05-23 | 状态: 待实施

---

## 1. 目标

1. **线程绑核**: 每个 worker 线程通过 hwloc 绑定到指定物理 CPU core
2. **NUMA 局部窃取**: work-stealing 仅在同 NUMA 节点内进行，禁止跨节点窃取
3. **NUMA 感知入队**: 外部提交 / `add_to_worker` 路由到目标 worker 所在 NUMA 节点

---

## 2. 当前拓扑

```
Machine (96GB)
├── Package L#0
│   ├── NUMANode L#0 (P#0, 31GB)
│   │   ├── Core L#0  → PU#0, PU#56
│   │   ├── Core L#1  → PU#1, PU#57
│   │   ├── ... (28 cores)
│   │   └── Core L#27 → PU#27, PU#83
│   └── NUMANode L#1 (P#1, 63GB)
│       ├── Core L#28 → PU#28, PU#84
│       ├── Core L#29 → PU#29, PU#85
│       ├── ... (28 cores)
│       └── Core L#55 → PU#55, PU#111
```

2 个 NUMA 节点，每节点 28 核心（56 HT），共 112 PUs。

---

## 3. 实现方案

### 3.1 WorkerState 扩展

```cpp
struct WorkerState {
    // ... existing fields ...

    int hwloc_cpu;          // 绑定的物理 PU ID (e.g. 0-111)
    int numa_node;          // 所属 NUMA 节点 (0 or 1)
    hwloc_cpuset_t cpuset;  // 绑核集合 (生命周期管理)
};

// NUMA node grouping
struct NumaGroup {
    int node_id;
    std::vector<size_t> worker_indices;  // worker IDs in this NUMA node
};
```

### 3.2 拓扑发现与绑核

```cpp
// hwloc discovery and topology building
struct Topology {
    hwloc_topology_t topo;
    int num_numa_nodes;
    std::vector<NumaGroup> numa_groups;
};

static Topology build_topology(size_t num_workers) {
    hwloc_topology_t topo;
    hwloc_topology_init(&topo);
    hwloc_topology_load(topo);

    int numa_count = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_NUMANODE);

    // 每个 NUMA 节点分配 workers_per_numa 个 worker
    size_t workers_per_numa = num_workers / numa_count;

    std::vector<NumaGroup> groups;
    for (int n = 0; n < numa_count; n++) {
        hwloc_obj_t node = hwloc_get_obj_by_type(topo, HWLOC_OBJ_NUMANODE, n);

        // 遍历该 NUMA 下的物理 core (跳过 HT sibling)
        std::vector<int> cores;
        for (int i = 0; i < node->arity; i++) {
            hwloc_obj_t child = node->children[i];
            if (hwloc_obj_type_is_cache(child->type)) continue;
            // child is Package → find Cores
            collect_physical_cores(child, cores);
        }

        NumaGroup group;
        group.node_id = n;
        // 从 cores 中均匀选取 workers_per_numa 个，绑定到其 PU#0
        for (size_t w = 0; w < workers_per_numa; w++) {
            size_t core_idx = (cores.size() * w) / workers_per_numa;
            group.worker_indices.push_back(...);
            // 绑定: hwloc_set_cpubind(topo, cpuset_of(core[core_idx]), HWLOC_CPUBIND_THREAD)
        }
        groups.push_back(group);
    }

    hwloc_topology_destroy(topo);
    return {.numa_groups = groups};
}
```

### 3.3 窃取限制

核心改动在 `worker_loop()` 的 Step 4:

```cpp
// 修改前 (L231-253):
size_t victim = fast_rand() % num_workers_;  // 任意 worker 都可能被窃

// 修改后:
// 只从同 NUMA 节点的 worker 窃取
auto& peers = numa_groups_[my_numa_node].worker_indices;
size_t victim_idx = fast_rand() % peers.size();
size_t victim = peers[victim_idx];
if (victim == my_id) {
    victim = peers[(victim_idx + 1) % peers.size()];
    if (victim == my_id) continue;  // NUMA 组内只有自己
}
```

### 3.4 NUMA 感知全局队列

每个 NUMA 节点有独立的全局队列，避免跨 NUMA 的 cache 失效:

```cpp
// 替换单个 global_queue_
std::vector<folly::UMPMCQueue<WorkItem, false, 6>> numa_queues_;

// add() 外部提交: 随机选一个 NUMA 队列 (或按 target_worker 所属 NUMA)
void add(folly::Func func) {
    int target_numa = (target_worker_id != kExternalThread)
        ? workers_[target_worker_id]->numa_node
        : (fast_rand() % numa_count_);
    numa_queues_[target_numa].enqueue(std::move(item));
    wake_one_worker(target_numa);  // 只唤醒目标 NUMA 的 worker
}
```

### 3.5 wake_one_worker 限制

```cpp
void wake_one_worker(int numa_node) {
    for (auto id : numa_groups_[numa_node].worker_indices) {
        auto& ws = workers_[id];
        std::lock_guard lock(ws->park_mutex);
        if (ws->parked.load(std::memory_order_acquire)) {
            ws->parked.store(false, std::memory_order_release);
            ws->park_cv.notify_one();
            return;
        }
    }
}
```

---

## 4. 实施方案

### 阶段 1: 基础绑核 (1d)
- 引入 hwloc 依赖（CMakeLists + `find_package(hwloc)`）
- `WorkerState` 添加 `hwloc_cpu` 和 `numa_node`
- 构造函数中调用 hwloc 发现拓扑 → 分配 worker → `hwloc_set_cpubind`
- 验证: `cat /proc/$(pidof quant_service)/task/*/status | grep Cpus_allowed`

### 阶段 2: NUMA 局部窃取 (0.5d)
- 将 `worker_loop` Step 4 的随机窃取改为同 NUMA 组内窃取
- 验证: 压测下观察 cross-NUMA steal 计数为 0

### 阶段 3: NUMA 队列 + 局部唤醒 (0.5d)
- `global_queue_` 拆为 `numa_queues_[]`
- `wake_one_worker` 加 NUMA 参数
- `add()` 中按 target_worker 的 NUMA 路由

---

## 5. 测试

- **绑定验证**: worker 线程确认运行在指定 CPU
- **窃取隔离**: 跨 NUMA steal 计数始终为 0
- **性能**: NUMA 感知 vs 原版的延迟/吞吐对比（预期同 NUMA 内延迟降低 30-50%）
- **负载均衡**: 同 NUMA 内 worker 利用率均衡度

---

## 6. 数据放置（后续优化）

当前设计只解决**计算 NUMA 本地化**。数据（KlineRow/ColumnBlock）在内存中的 NUMA 放置是下一步优化：
- `TimeSeriesCache` 的 shard 分配到特定 NUMA 节点
- 写入线程与读取线程同 NUMA，避免远端内存访问
- 可结合 hwloc 的 `hwloc_alloc_membind` 分配 NUMA 绑定内存

---

## 7. 风险和注意事项

1. **HT siblings**: 绑核时只绑物理 core 的 PU#0，不绑 HT sibling PU#1，避免两个 worker 共享同一个 core 的 L1/L2 cache
2. **NUMA 负载不均**: 如果任务天然偏向某个 NUMA，需要在 add() 时做 better routing
3. **hwloc 内存泄漏**: cpuset 和 topology 对象需在析构函数中正确释放
