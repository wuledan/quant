# 协程化改造 — 细致开发及测试规划

> 日期: 2026-05-18
> 前置文档: `coroutine_refactor_discussion.md`（完整讨论记录与架构决策）

## 设计约束（来自讨论）

1. **I/O 全量 io_uring 化** — 所有磁盘/网络 I/O 非阻塞，用 Folly 的 IoUringBackend
2. **慢降级、快恢复** — worker 空闲时 spin→yield→futex 三级退避，任务到达立即唤醒
3. **线程亲和性** — DAG 提交任务优先到当前线程的 LocalDeque，定时器到期协程回到 last_worker_id
4. **仅单机多线程** — 不考虑多进程，删除 Remote Stealing
5. **全局线程 + timer loop** — 定时器用 HHWheelTimer，到期后协程回到上次执行线程
6. **Baton 跨线程同步经 executor 路由** — post 时不直接 resume，通过 add_to_worker 路由
7. **全局统计类** — 所有计数器/延迟统一用 StatRegistry

---

## WorkStealingExecutor 详细内部设计

> 本节回答：在我们自行实现的 executor 中，如何**完整体现**以上 7 条设计约束。
> 从数据结构到调度决策，从线程身份到协程恢复路由，给出端到端的完整方案。

### 1. 核心数据结构

#### 1.1 WorkItem — 任务类型擦除

```cpp
// work_stealing_executor.h

class WorkStealingExecutor : public folly::Executor {
    // 内部任务单元
    struct WorkItem {
        folly::Function<void()> func;   // 类型擦除的可调用对象

        // 亲和性元数据（可选）
        size_t target_worker_id = kNoAffinity;  // kNoAffinity = SIZE_MAX 表示无亲和性要求
        bool is_coroutine_resume = false;        // 是否为协程恢复（影响统计）

        void execute() noexcept {
            if (func) {
                func();
                func = nullptr;  // 释放捕获的资源
            }
        }
        explicit operator bool() const noexcept { return static_cast<bool>(func); }
    };

    static constexpr size_t kNoAffinity = SIZE_MAX;
    // ...
};
```

**为什么 WorkItem 携带 `target_worker_id`？**
- `add(Func)` 从 pool worker 调用时：`target_worker_id = current_worker_id()`，任务优先入当前 worker 的 LocalDeque
- `add_to_worker(id, Func)` 调用时：`target_worker_id = id`，定向到指定 worker
- 外部线程调用 `add(Func)` 时：`target_worker_id = kNoAffinity`，入 GlobalQueue
- Worker 从 GlobalQueue 取到 `target_worker_id != kNoAffinity` 的任务时，若 target 不是自己，可以转投到该 worker 的 LocalDeque（优化，非必须）

#### 1.2 WorkerState — 每个 worker 的完整状态

```cpp
struct WorkerState {
    // ── 任务队列 ──
    std::unique_ptr<ChaseLevDeque<WorkItem>> local_deque;  // lock-free, owner 独占 push/pop

    // ── 线程身份 ──
    std::thread thread;                          // worker 线程句柄
    size_t worker_id;                            // 本 worker 的 ID（0 ~ N-1）
    std::atomic<bool> parked{false};             // 是否在 futex park 中

    // ── 三级退避状态 ──
    struct ParkState {
        std::atomic<uint32_t> spin_count{0};     // 当前自旋计数
        std::atomic<uint32_t> yield_count{0};    // 当前 yield 计数
        bool in_futex{false};                    // 是否进入 futex park

        // 参数（可调）
        static constexpr uint32_t kSpinMaxUs = 10;     // 自旋上限 μs
        static constexpr uint32_t kYieldRounds = 3;    // yield 轮数
    } park_state;

    // ── 利用率追踪 ──
    std::atomic<uint64_t> busy_cycles_ns{0};     // 执行任务累计纳秒
    std::atomic<uint64_t> total_cycles_ns{0};    // worker_loop 运行累计纳秒

    // ── 统计快照 ──
    std::atomic<uint64_t> local_pops{0};
    std::atomic<uint64_t> steals_from_me{0};     // 被其他 worker 偷取次数
};
```

#### 1.3 GlobalMPMCQueue — 外部提交和溢出

```cpp
// 基于 folly::UMPMCQueue 或自建的 MPMC 无锁队列
// 选择: 直接使用 folly::UMPMCQueue<WorkItem>
//
// 容量: 2 * num_workers * local_queue_capacity
// 场景:
//   - 外部线程通过 add() 提交的任务
//   - LocalDeque 满时溢出（Chase-Lev 不支持容量限制，故不溢出；此队列仅服务外部提交）
//
// Worker 取任务顺序: LocalDeque → GlobalMPMC → steal
```

#### 1.4 Executor 整体布局

```cpp
class WorkStealingExecutor : public folly::Executor {
public:
    // ... 公共接口见下文 ...

private:
    // ── Worker 数组 ──
    std::vector<std::unique_ptr<WorkerState>> workers_;
    size_t num_workers_;

    // ── 全局队列 ──
    folly::UMPMCQueue<WorkItem> global_queue_;

    // ── 偷窃随机源 ──
    // 每个 worker 有自己的随机数生成器，避免共享竞争
    // 使用 thread_local 或 WorkerState 内嵌

    // ── 生命周期 ──
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};  // 优雅停止：等待所有任务完成

    // ── 唤醒机制 ──
    // 每个 worker 的 futex: workers_[i]->park_state
    // 唤醒方式: futex_wake(&workers_[i]->park_state, 1)
    // 全局唤醒: 遍历所有 parked worker 逐个 futex_wake

    // ── Folly Executor::KeepAlive 支持 ──
    // folly::Executor::KeepAlive<> 要求 executor 继承 folly::Executor
    // 并实现 virtual void add(Func) override
    // KeepAlive 的引用计数由 Folly 内部管理

    // ── 统计键前缀 ──
    static constexpr const char* kStatPrefix = "executor.";
};
```

### 2. 线程身份机制

#### 2.1 thread_local worker_id

```cpp
// work_stealing_executor.cc

namespace {
    // 每个 worker 线程的本地 ID
    thread_local size_t tl_worker_id = kExternalThread;
    thread_local WorkStealingExecutor* tl_executor = nullptr;

    constexpr size_t kExternalThread = SIZE_MAX;
}

size_t WorkStealingExecutor::current_worker_id() {
    return tl_worker_id;
}

bool WorkStealingExecutor::is_pool_worker() {
    return tl_worker_id != kExternalThread;
}
```

#### 2.2 身份的设置与读取时机

```
设置时机:
  worker_loop() 入口处:
    tl_worker_id = my_worker_id;
    tl_executor = this;
    // ... loop ...
    tl_worker_id = kExternalThread;  // 退出前清除

读取时机:
  1. add(Func) — 判断调用者是否是 pool worker，决定任务入哪个队列
  2. AffinityBaton::await_suspend — 记录当前 worker_id 作为 last_worker_id
  3. TimerScheduler::co_sleep — 记录调用者的 worker_id
  4. ExecutionDAG::co_execute — 提交子任务时获取当前 worker_id
  5. StatRegistry — thread_local LocalHolder 自动关联当前线程
```

#### 2.3 last_worker_id 的传播路径

```
协程 A 在 worker-2 上执行:
  tl_worker_id = 2

  co_await timer.co_sleep(100ms)
    → await_suspend 时捕获: last_worker_id = tl_worker_id = 2
    → 协程 A 挂起

  ... 100ms 后 ...

  TimerScheduler 的 HHWheelTimer callback 触发:
    → 读取 last_worker_id = 2
    → executor.add_to_worker(2, continuation_A)
    → 协程 A 被 push 到 workers_[2]->local_deque
    → worker-2 下一轮 pop 拿到 → resume 协程 A
    → resume 时 tl_worker_id = 2（在 worker-2 的 loop 中）
```

### 3. add() 与 add_to_worker() 的调度决策

#### 3.1 add(Func) — Folly coro 自动调用

```cpp
void WorkStealingExecutor::add(Func func) override {
    StatRegistry::instance().increment(kStatPrefix + "tasks_submitted");

    WorkItem item;
    item.func = std::move(func);
    item.is_coroutine_resume = true;  // Folly 的 add() 调用都是协程恢复

    if (tl_executor == this) {
        // 场景 1: pool worker 调用 add()
        //   Folly coro 的 co_viaIfAsync 在 co_await 返回后调用 executor_->add(continuation)
        //   此时调用者是 pool worker，continuation 应该回到该 worker 的 LocalDeque
        //   线程亲和性: 任务留在当前 worker，cache 最优
        item.target_worker_id = tl_worker_id;
        workers_[tl_worker_id]->local_deque->push(std::move(item));

        // 如果有其他 worker 在 park，可能需要唤醒一个（因为当前 worker 可能马上要执行
        // 这个 continuation 而不是再取新任务——其实不需要额外唤醒，当前 worker 会继续 loop）
    } else {
        // 场景 2: 外部线程调用 add()
        //   例如 blockingWait 桥接、TimerScheduler 的全局线程等
        //   无亲和性信息，入 GlobalQueue
        item.target_worker_id = kNoAffinity;
        global_queue_.write(std::move(item));

        // 唤醒一个 parked worker 来取任务
        wake_one_worker();
    }
}
```

**关键洞察**: Folly coro 的 `co_viaIfAsync` 保证了 executor 亲和性——协程恢复时调用 `executor_->add(continuation)`。我们的 `add()` 实现**在此基础上进一步**保证了线程亲和性：如果 `add()` 被一个 pool worker 调用（协程恢复的场景），任务直接进入该 worker 的 LocalDeque，保证该 worker 下一轮 pop 拿到。

#### 3.2 add_to_worker(worker_id, Func) — 定向投递

```cpp
void WorkStealingExecutor::add_to_worker(size_t worker_id, Func func) {
    StatRegistry::instance().increment(kStatPrefix + "tasks_submitted");

    WorkItem item;
    item.func = std::move(func);
    item.target_worker_id = worker_id;
    item.is_coroutine_resume = true;

    // 直接 push 到目标 worker 的 LocalDeque
    workers_[worker_id]->local_deque->push(std::move(item));

    // 如果目标 worker 在 futex park 中，唤醒它
    if (workers_[worker_id]->parked.load(std::memory_order_acquire)) {
        futex_wake(&workers_[worker_id]->park_state, 1);
        StatRegistry::instance().increment(kStatPrefix + "unpark_count");
    }
}
```

**使用场景**:
- TimerScheduler 到期后: `add_to_worker(last_worker_id, continuation)`
- AffinityBaton post 后: `add_to_worker(last_worker_id, resume_func)`
- ExecutionDAG 同层任务提交: `add_to_worker(current_worker_id, task_func)`

#### 3.3 wake_one_worker() — 选择性唤醒

```cpp
void WorkStealingExecutor::wake_one_worker() {
    // 策略: 唤醒第一个 parked 的 worker
    // 更优策略: 随机选择一个 parked worker（减少 thundering herd）
    for (auto& ws : workers_) {
        if (ws->parked.load(std::memory_order_acquire)) {
            futex_wake(&ws->park_state, 1);
            StatRegistry::instance().increment(kStatPrefix + "unpark_count");
            return;
        }
    }
    // 所有 worker 都在运行，无需唤醒
}
```

### 4. Worker Loop — 三级退避完整伪代码

```cpp
void WorkStealingExecutor::worker_loop(size_t my_id) {
    tl_worker_id = my_id;
    tl_executor = this;

    auto& me = *workers_[my_id];
    auto& park = me.park_state;
    auto rng = fastrng(my_id);  // 每个 worker 独立的随机数生成器

    auto loop_start = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        WorkItem item;
        bool found = false;

        // ── 第 1 步: pop 自己的 LocalDeque (O(1), 无锁) ──
        auto popped = me.local_deque->pop();
        if (popped) {
            item = std::move(*popped);
            found = true;
            StatRegistry::instance().increment(kStatPrefix + "local_pops");
        }

        // ── 第 2 步: 从 GlobalQueue 取 ──
        if (!found) {
            WorkItem global_item;
            if (global_queue_.read(global_item)) {
                // 如果该任务有亲和性要求且目标不是自己，转投
                if (global_item.target_worker_id != kNoAffinity &&
                    global_item.target_worker_id != my_id) {
                    workers_[global_item.target_worker_id]->local_deque->push(
                        std::move(global_item));
                    wake_one_worker();  // 可能需要唤醒目标 worker
                    continue;  // 重新取任务
                }
                item = std::move(global_item);
                found = true;
                StatRegistry::instance().increment(kStatPrefix + "global_queue_pops");
            }
        }

        // ── 第 3 步: 随机偷其他 worker 的 Deque (只偷 top, FIFO) ──
        if (!found) {
            constexpr size_t kStealAttempts = 4;
            for (size_t attempt = 0; attempt < kStealAttempts; ++attempt) {
                size_t victim = rng() % num_workers_;
                if (victim == my_id) continue;

                auto stolen = workers_[victim]->local_deque->steal();
                if (stolen) {
                    item = std::move(*stolen);
                    found = true;
                    workers_[victim]->steals_from_me.fetch_add(1,
                        std::memory_order_relaxed);
                    StatRegistry::instance().increment(kStatPrefix + "local_steals");
                    break;
                }
            }
            if (!found) {
                StatRegistry::instance().increment(kStatPrefix + "failed_steals");
            }
        }

        // ── 第 4 步: 执行任务 or 三级退避 park ──
        if (found) {
            // 重置退避状态
            park.spin_count.store(0, std::memory_order_relaxed);
            park.yield_count.store(0, std::memory_order_relaxed);

            // 执行
            auto task_start = std::chrono::steady_clock::now();
            item.execute();
            auto task_end = std::chrono::steady_clock::now();

            // 利用率追踪
            auto busy_ns = (task_end - task_start).count();
            me.busy_cycles_ns.fetch_add(busy_ns, std::memory_order_relaxed);

            StatRegistry::instance().increment(kStatPrefix + "tasks_completed");
            if (item.is_coroutine_resume) {
                StatRegistry::instance().increment(kStatPrefix + "handles_resumed");
            }
            StatRegistry::instance().observe_timer(
                kStatPrefix + "task_latency_us", busy_ns / 1000.0);

        } else {
            // ── 三级退避: 慢降级 ──
            auto spin = park.spin_count.load(std::memory_order_relaxed);
            auto yield_cnt = park.yield_count.load(std::memory_order_relaxed);

            if (spin < ParkState::kSpinMaxUs) {
                // 级别 1: PAUSE 自旋
                //   耗电低、对超线程友好、延迟最低
                //   每次 PAUSE 约 100-200ns，累加到上限
                #if defined(__x86_64__)
                asm volatile("pause");
                #elif defined(__aarch64__)
                asm volatile("yield");
                #endif
                park.spin_count.fetch_add(1, std::memory_order_relaxed);

            } else if (yield_cnt < ParkState::kYieldRounds) {
                // 级别 2: yield
                //   让出 CPU 时间片，OS 调度器可以给其他线程
                //   切换开销 ~1-10μs
                std::this_thread::yield();
                park.yield_count.fetch_add(1, std::memory_order_relaxed);

            } else {
                // 级别 3: futex park
                //   内核调度睡眠，CPU 利用率 ~0%
                //   唤醒开销: futex_wake → 内核调度 → 线程恢复 ≈ 5-50μs
                //   这是最终退避态，只有 add/add_to_worker 会唤醒
                me.parked.store(true, std::memory_order_release);
                StatRegistry::instance().increment(kStatPrefix + "park_count");

                futex_wait(&park, /*expected=*/1);  // 阻塞直到 futex_wake

                me.parked.store(false, std::memory_order_release);

                // 唤醒后重置，下次空闲重新从自旋开始
                park.spin_count.store(0, std::memory_order_relaxed);
                park.yield_count.store(0, std::memory_order_relaxed);
            }

            // 空闲周期追踪
            me.total_cycles_ns.fetch_add(
                (std::chrono::steady_clock::now() - loop_start).count(),
                std::memory_order_relaxed);
        }
    }

    tl_worker_id = kExternalThread;
    tl_executor = nullptr;
}
```

**慢降级、快恢复的完整体现**:

```
降级路径（慢降级）:
  自旋 PAUSE → spin_count 递增到 10μs → yield 3 轮 → futex park
  每一级的退出条件是"取到任务"，否则继续降级
  降级过程本身不快（10μs 自旋 + 3 次 yield + futex 开销），
  确保不会在短暂空闲时过早进入深度睡眠

恢复路径（快恢复）:
  futex park 的 worker 被 futex_wake 唤醒后:
    1. 从 futex_wait 返回（内核调度，~5-50μs）
    2. 重置退避计数器
    3. 回到 loop 顶部，立即 pop LocalDeque
    4. 如果 add_to_worker 刚投递了任务 → pop 命中 → 立即执行
    5. 从 futex 唤醒到开始执行任务: 仅一次 pop 操作（O(1)）

  对于自旋/yield 状态的 worker:
    任务到达 → 下一次 loop 迭代立即拿到（< 1μs 延迟）
```

### 5. Folly coro::Task 恢复的完整路由路径

理解 `add()` 如何被 Folly 调用，是整个设计的核心。

```
用户代码:
  CoTask<int> my_coroutine() {
    auto val = co_await executor.co_submit(some_func);  // (1) 提交任务
    co_await CoBaton{};                                  // (2) 等待 Baton
    co_await timer.co_sleep(100ms);                      // (3) 定时等待
    co_return val;
  }

Folly 内部变换（co_viaIfAsync）:
  每个 co_await 被编译器变换为:
    auto&& awaitable = co_await_transform(expr);
    auto&& via_awaitable = co_viaIfAsync(executor_.get(), std::forward<decltype(awaitable)>(awaitable));
    auto result = co_await via_awaitable;

  co_viaIfAsync 的工作:
    1. 当前协程挂起
    2. 当 awaitable 完成时，ViaCoroutine 的回调被触发
    3. ViaCoroutine 调用 executor_->add(continuation)
    4. continuation 在 executor 的某个 worker 上 resume

  这就是 Folly 保证 executor 亲和性的机制。
  我们在 add() 中进一步保证线程亲和性。
```

**各场景的完整路由**:

```
场景 1: co_submit → 任务完成 → 调用者恢复
  ┌─────────────────────────────────────────────────────────────┐
  │ worker-2 执行 my_coroutine                                  │
  │   co_await executor.co_submit(func)                         │
  │     → await_suspend: 挂起 my_coroutine                     │
  │     → func 被 enqueue 到 GlobalQueue (外部提交模式)          │
  │       或 LocalDeque (pool worker 提交模式)                   │
  │                                                             │
  │   func 在 worker-3 上执行完成                               │
  │     → CoTaskState::completed = true                         │
  │     → CoTaskState::waiter 上的 continuation 被封装为 WorkItem│
  │     → executor.add(continuation)                            │
  │       此时 add() 的调用者是 worker-3                        │
  │       → tl_worker_id = 3                                    │
  │       → continuation 入 workers_[3]->local_deque            │
  │                                                             │
  │   ⚠️ 问题: 原始调用者是 worker-2，但恢复在 worker-3!        │
  │   这是因为 co_submit 的 wrapper 完成后调用 add(continuation)│
  │   而 add() 的线程亲和性是基于调用者线程的。                  │
  └─────────────────────────────────────────────────────────────┘

  解决: co_submit 不使用 Folly 的 ViaCoroutine 路由。
  改为在 CoTaskState 中记录 caller_worker_id，任务完成后
  使用 add_to_worker(caller_worker_id, continuation)。

  修改后的流程:
    co_await executor.co_submit(func)
      → await_suspend 时: state->caller_worker_id = tl_worker_id
      → func 在任意 worker 上执行完成
      → wrapper 完成: executor.add_to_worker(state->caller_worker_id, continuation)
      → continuation 在 caller 的 worker 上恢复 ✓
```

```
场景 2: co_await CoBaton → post → 恢复
  ┌─────────────────────────────────────────────────────────────┐
  │ 使用原生 folly::coro::Baton:                                │
  │   worker-1: co_await baton  → 挂起                          │
  │   worker-3: baton.post()    → 直接在 worker-3 上 resume     │
  │   ❌ 协程跑到了 worker-3，不符合线程亲和性                   │
  │                                                             │
  │ 使用 AffinityBaton:                                         │
  │   worker-1: co_await affinity_baton                         │
  │     → await_suspend 时: last_worker_id = tl_worker_id = 1  │
  │     → 协程挂起                                              │
  │   worker-3: affinity_baton.post(executor)                   │
  │     → 不直接 resume，而是:                                   │
  │     → executor.add_to_worker(1, resume_coroutine)           │
  │     → resume_coroutine 入 workers_[1]->local_deque          │
  │     → worker-1 下一轮 pop 拿到 → resume 协程 ✓              │
  └─────────────────────────────────────────────────────────────┘
```

```
场景 3: co_await timer.co_sleep(100ms) → 到期 → 恢复
  ┌─────────────────────────────────────────────────────────────┐
  │ worker-2: co_await timer.co_sleep(100ms)                    │
  │   → TimerScheduler::co_sleep 的 await_suspend:              │
  │     1. last_worker_id = tl_worker_id = 2                    │
  │     2. 向 HHWheelTimer 注册定时器，闭包捕获:                │
  │        - continuation (协程句柄)                             │
  │        - last_worker_id = 2                                 │
  │     3. 协程挂起                                             │
  │                                                             │
  │   100ms 后，TimerScheduler 的全局线程:                       │
  │     → HHWheelTimer callback 触发                            │
  │     → 闭包执行:                                             │
  │       executor.add_to_worker(2, continuation)               │
  │     → continuation 入 workers_[2]->local_deque              │
  │     → 如果 worker-2 在 futex park，futex_wake 唤醒          │
  │     → worker-2 pop 拿到 → resume 协程 ✓                     │
  └─────────────────────────────────────────────────────────────┘
```

```
场景 4: collectAll 并行子任务 → 全部完成 → 调用者恢复
  ┌─────────────────────────────────────────────────────────────┐
  │ worker-2: co_await collectAll(task_a, task_b, task_c)       │
  │   → Folly 的 collectAll 实现:                               │
  │     1. 启动所有子任务（各自 co_withExecutor 到 executor）    │
  │     2. 每个子任务完成后调用 executor.add(continuation)       │
  │     3. 最后一个完成的子任务触发 aggregate continuation        │
  │     4. aggregate continuation 通过 add() 回到 executor      │
  │                                                             │
  │   线程亲和性分析:                                           │
  │     - 子任务启动: 各自在不同 worker 上运行（由调度决定）     │
  │     - 子任务完成时 add(continuation):                        │
  │       调用者是完成子任务的 worker → continuation 入该 worker │
  │     - 这不保证回到 worker-2                                 │
  │                                                             │
  │   改进: 对 collectAll 的 aggregate continuation，           │
  │   我们不额外处理——因为 collectAll 的结果聚合本身是轻量的，  │
  │   在哪个 worker 上恢复影响不大。如果需要严格亲和性，         │
  │   可以在 collectAll 后加 co_await co_reschedule_on(this)    │
  │   但这增加一次调度开销，通常不值得。                         │
  │                                                             │
  │   结论: collectAll 子任务的亲和性不做额外保证，              │
  │   依赖 Folly 的 executor 亲和性（回到同一个 executor 实例）  │
  │   但不保证回到同一个 worker 线程。                           │
  └─────────────────────────────────────────────────────────────┘
```

### 6. co_submit 的完整实现

co_submit 是最复杂的原语——它需要保证调用者恢复到原来的 worker。

```cpp
template<typename F>
folly::coro::Task<std::invoke_result_t<F>>
WorkStealingExecutor::co_submit(F&& func) {
    using R = std::invoke_result_t<F>;

    // 协程在 pool worker 上执行时，co_submit 的调用者就是这个 worker
    // 我们需要在任务完成后，把调用者恢复到这个 worker
    // 方法: 利用 co_await 的 await_suspend 捕获当前 worker_id，
    //       然后让 wrapper 通过 add_to_worker 路由回来

    struct CoSubmitAwaiter {
        std::shared_ptr<folly::Promise<R>> promise;
        folly::Future<R> future;
        size_t caller_worker_id;
        WorkStealingExecutor* executor;

        bool await_ready() const noexcept { return future.isReady(); }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            // 记录调用者的 worker_id
            caller_worker_id = tl_worker_id;

            // future 完成后的回调
            std::move(future).thenTry([handle, caller_worker_id, executor = executor](
                folly::Try<R> result) {
                // 任务完成，通过 add_to_worker 路由回调用者的 worker
                executor->add_to_worker(caller_worker_id, [handle]() mutable {
                    handle.resume();
                });
            });
        }

        R await_resume() {
            return std::move(future).get();
        }
    };

    // 提交任务到 executor
    auto promise = std::make_shared<folly::Promise<R>>();
    auto future = promise->getFuture();

    auto caller_worker_id = tl_worker_id;
    auto* executor = this;

    // 包装任务: 在 executor 上执行 func，完成后通过 promise 通知
    this->add([func = std::forward<F>(func), promise]() mutable {
        try {
            if constexpr (std::is_void_v<R>) {
                func();
                promise->setValue();
            } else {
                promise->setValue(func());
            }
        } catch (...) {
            promise->setException(std::current_exception());
        }
    });

    // co_await 等待结果，恢复时通过 add_to_worker 回到 caller_worker_id
    auto result = co_await CoSubmitAwaiter{
        /*.promise = */ std::move(promise),
        /*.future = */ std::move(future),
        /*.caller_worker_id = */ caller_worker_id,
        /*.executor = */ executor
    };

    co_return result;
}
```

**简化方案**: 实际上可以使用更简单的方式——不自己实现 awaiter，而是利用 Folly 的 `co_await co_viaIfAsync`:

```cpp
template<typename F>
folly::coro::Task<std::invoke_result_t<F>>
WorkStealingExecutor::co_submit(F&& func) {
    using R = std::invoke_result_t<F>;

    // 在 executor 上执行 func
    auto result = co_await folly::coro::co_viaIfAsync(
        folly::getKeepAliveToken(this),
        folly::coro::co_invoke([func = std::forward<F>(func)]() mutable -> folly::coro::Task<R> {
            co_return func();
        })
    );

    co_return result;
}
```

**但这只保证 executor 亲和性，不保证线程亲和性。** 最终选择：使用第一种方案（自定义 CoSubmitAwaiter + add_to_worker）来保证调用者的线程亲和性。

### 7. AffinityBaton 完整设计（多等待者 + executor 路由）

#### 7.1 设计目标

AffinityBaton 是 `folly::coro::Baton` 的完整替代品，具备 Baton 的全部能力：

| 能力 | folly::coro::Baton | AffinityBaton |
|------|-------------------|---------------|
| 协程等待 (`co_await`) | ✅ | ✅ |
| 多等待者 | ✅ 原子 CAS 链表 | ✅ 侵入式链表 |
| `post()` 唤醒所有等待者 | ✅ | ✅ |
| `ready()` 查询 | ✅ | ✅ |
| `reset()` 重置 | ✅ | ✅ |
| `try_wait()` 非阻塞检查 | ✅ | ✅ |
| post 后走 executor 路由 | ❌ 直接 resume | ✅ add_to_worker |
| 线程亲和性 | ❌ | ✅ |

**关键区别**: `folly::coro::Baton::post()` 直接在调用线程上 `resume()` 等待协程，绕过 executor。AffinityBaton 的 `post()` 通过 `executor.add_to_worker(waiter_worker_id, continuation)` 路由，保证协程在原 worker 上恢复。

#### 7.2 为什么不包装 folly::coro::Baton

`folly::coro::Baton::post()` 内部实现：

```cpp
// folly/coro/Baton.cpp (简化)
void Baton::post() noexcept {
    // 原子 CAS 将状态从 NOTREADY 改为 READY
    auto* awaiters = waiterList_.exchange(nullptr, std::memory_order_acq_rel);
    // 直接在当前线程上 resume 所有等待者
    while (awaiters) {
        auto* next = awaiters->next;
        awaiters->awaitingCoroutine.resume();  // ← 直接 resume！不走 executor
        awaiters = next;
    }
}
```

这个 `resume()` 调用无法被拦截或重定向。因此必须自建等待链，不能包装 Baton。

#### 7.3 完整实现

```cpp
// affinity_baton.h

#pragma once
#include <atomic>
#include <coroutine>
#include <cstddef>

namespace quant::infra {

class WorkStealingExecutor;  // forward decl

// ── AffinityBaton ──
//
// 完整替代 folly::coro::Baton，具备:
//   - 多等待者支持（侵入式链表）
//   - post() 后通过 executor 路由，不直接 resume
//   - 线程亲和性: 每个等待者记录自己的 worker_id，post 后各回到原 worker
//
class AffinityBaton {
public:
    AffinityBaton() noexcept = default;
    ~AffinityBaton() = default;

    // 不可拷贝/移动
    AffinityBaton(const AffinityBaton&) = delete;
    AffinityBaton& operator=(const AffinityBaton&) = delete;

    // ── 状态查询 ──

    bool ready() const noexcept {
        return state_.load(std::memory_order_acquire) == State::POSTED;
    }

    // 非阻塞尝试等待，返回是否已经 posted
    bool try_wait() const noexcept {
        return ready();
    }

    // 重置为未 post 状态（仅在确认无等待者时调用）
    void reset() noexcept {
        state_.store(State::NOT_READY, std::memory_order_release);
    }

    // ── 侵入式等待节点 ──
    struct WaiterNode {
        std::coroutine_handle<> handle;
        size_t worker_id;           // 等待者所在的 worker_id
        WaiterNode* next;           // 链表 next 指针
    };

    // ── 协程等待 ──
    struct Awaiter {
        AffinityBaton& baton;
        WaiterNode node;

        bool await_ready() const noexcept {
            return baton.ready();
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            node.handle = handle;
            node.worker_id = WorkStealingExecutor::current_worker_id();
            node.next = nullptr;

            // CAS 循环: 将 node 加入 waiters_ 链表头部
            auto* old_head = baton.waiters_.load(std::memory_order_acquire);

            do {
                // double-check: 如果在加入链表前已经 post
                if (baton.ready()) {
                    // 已经 posted，直接 resume
                    // 此时 post 尚未调用，无 executor 可路由
                    handle.resume();
                    return;
                }
                node.next = old_head;
            } while (!baton.waiters_.compare_exchange_weak(
                old_head, &node,
                std::memory_order_release,
                std::memory_order_acquire));

            // 成功加入等待链，协程挂起
        }

        void await_resume() const noexcept {}
    };

    Awaiter operator co_await() noexcept {
        return Awaiter{*this, WaiterNode{}};
    }

    // ── post: 通过 executor 路由，唤醒所有等待者 ──
    void post(WorkStealingExecutor& executor);

    // ── 不带 executor 的 post（直接 resume，仅用于无 executor 的场景） ──
    //   行为同 folly::coro::Baton::post()，不保证线程亲和性
    //   用于: 析构时唤醒、测试代码等
    void post_direct() noexcept;

private:
    // 恢复等待链上的所有协程
    // executor != nullptr: 通过 add_to_worker 路由
    // executor == nullptr: 直接 resume
    void resume_chain(WaiterNode* waiters, WorkStealingExecutor* executor);

    enum class State : uint8_t {
        NOT_READY,
        POSTED,
    };

    std::atomic<State> state_{State::NOT_READY};
    std::atomic<WaiterNode*> waiters_{nullptr};
};

}  // namespace quant::infra
```

```cpp
// affinity_baton.cc

#include "affinity_baton.h"
#include "work_stealing_executor.h"

namespace quant::infra {

void AffinityBaton::post(WorkStealingExecutor& executor) {
    // 原子交换: 取出整个等待链
    auto* waiters = waiters_.exchange(nullptr, std::memory_order_acq_rel);
    state_.store(State::POSTED, std::memory_order_release);

    resume_chain(waiters, &executor);
}

void AffinityBaton::post_direct() noexcept {
    auto* waiters = waiters_.exchange(nullptr, std::memory_order_acq_rel);
    state_.store(State::POSTED, std::memory_order_release);

    resume_chain(waiters, nullptr);
}

void AffinityBaton::resume_chain(WaiterNode* waiters,
                                  WorkStealingExecutor* executor) {
    while (waiters) {
        auto* next = waiters->next;
        auto handle = waiters->handle;
        auto worker_id = waiters->worker_id;

        if (executor && worker_id != SIZE_MAX) {
            // 通过 add_to_worker 路由到等待者的原 worker
            executor->add_to_worker(worker_id, [handle]() mutable {
                handle.resume();
            });
        } else {
            // 无 executor 或无 worker_id（外部线程等待），直接 resume
            handle.resume();
        }

        waiters = next;
    }
}

}  // namespace quant::infra
```

#### 7.4 多等待者场景示例

```
场景: 3 个协程等待同一个 AffinityBaton

  worker-1: co_await baton  → node1 = {handle_A, worker_id=1, next=null}
  worker-3: co_await baton  → node2 = {handle_B, worker_id=3, next=&node1}
  worker-0: co_await baton  → node3 = {handle_C, worker_id=0, next=&node2}

  waiters_ 链表: node3 → node2 → node1

  worker-2: baton.post(executor)
    → 取出整个链表
    → 遍历链表:
        node3: executor.add_to_worker(0, handle_C)  → worker-0 resume handle_C
        node2: executor.add_to_worker(3, handle_B)  → worker-3 resume handle_B
        node1: executor.add_to_worker(1, handle_A)  → worker-1 resume handle_A
    → 所有等待者回到各自原来的 worker 线程 ✓

  对比 folly::coro::Baton::post():
    → 在 worker-2 上依次 resume handle_C, handle_B, handle_A
    → 三个协程全在 worker-2 上执行 ✗
```

#### 7.5 与 folly::coro::Baton 的 API 对比

```cpp
// folly::coro::Baton 的完整 API:
baton.ready()           // 查询
baton.try_wait()        // 非阻塞等待
baton.reset()           // 重置
co_await baton          // 协程等待（多等待者）
baton.post()            // 唤醒所有等待者

// AffinityBaton 的完整 API（1:1 对齐 + 亲和性增强）:
baton.ready()           // 查询                    ✅ 相同
baton.try_wait()        // 非阻塞等待               ✅ 相同
baton.reset()           // 重置                    ✅ 相同
co_await baton          // 协程等待（多等待者）      ✅ 相同
baton.post(executor)    // 唤醒所有等待者，走 executor 路由  ✅ 增强
baton.post_direct()     // 唤醒所有等待者，直接 resume       ✅ 兼容
```

#### 7.6 Awaiter 的栈上分配与生命周期安全

`WaiterNode` 是 `Awaiter` 的成员，而 `Awaiter` 是 `operator co_await()` 返回的临时对象。在 C++ 协程 ABI 中，awaiter 对象的生命周期覆盖 `await_suspend` → `await_resume`，因此 `WaiterNode` 在协程挂起期间是有效的。

```
时序保证:
  await_suspend(): 将 &node 加入 waiters_ 链表，然后协程挂起
  post(): 取出链表，读取 node.handle 和 node.worker_id，投递到 executor
          投递后不再访问 node
  await_resume(): 协程恢复，awaiter 析构，node 失效

  关键: post() 在读取 node 数据后立即投递，不再引用 node。
        await_resume() 在 resume 之后才执行，此时 post() 早已完成。
        因此 node 的生命周期是安全的。
```

### 8. TimerScheduler 完整设计

```cpp
// timer_scheduler.h

class TimerScheduler {
public:
    explicit TimerScheduler(WorkStealingExecutor& executor)
        : executor_(executor) {}

    void start() {
        // 启动全局定时器线程
        timer_thread_ = std::thread([this]() {
            // EventBase 使用 IoUringBackend（满足 I/O 非阻塞约束）
            auto backend_factory = []() -> std::unique_ptr<folly::EventBaseBackendBase> {
                return std::make_unique<folly::IoUringBackend>(
                    folly::IoUringBackend::Options{}
                );
            };
            event_base_ = std::make_unique<folly::EventBase>(
                folly::EventBase::Options().setBackendFactory(backend_factory)
            );

            // 设置 HHWheelTimer
            wheel_timer_ = folly::HHWheelTimer::newTimer(
                event_base_.get(),
                std::chrono::milliseconds(1)  // tick interval
            );

            // 运行 EventBase loop
            event_base_->loopForever();
        });
        timer_thread_.set_name("quant-timer");
    }

    void stop() {
        if (event_base_) {
            event_base_->runInEventBaseThread([this]() {
                wheel_timer_->cancelAll();
                event_base_->terminateLoopSoon();
            });
        }
        if (timer_thread_.joinable()) {
            timer_thread_.join();
        }
    }

    // ── 协程定时 ──
    folly::coro::Task<void> co_sleep(std::chrono::nanoseconds duration) {
        // 捕获当前 worker_id
        size_t caller_worker_id = WorkStealingExecutor::current_worker_id();

        // 使用 folly::coro::sleep + co_viaIfAsync
        // 但我们不用 Folly 默认的 sleep（它不保证线程亲和性）
        // 而是手动实现:
        auto promise = std::make_shared<folly::Promise<folly::Unit>>();
        auto future = promise->getFuture();

        // 在 timer 线程上注册定时器
        event_base_->runInEventBaseThread([this, duration, promise, caller_worker_id]() {
            wheel_timer_->scheduleTimeoutFn(
                [this, promise, caller_worker_id]() {
                    // 定时器到期 → 通过 add_to_worker 路由回调用者的 worker
                    executor_.add_to_worker(caller_worker_id, [promise]() {
                        promise->setValue(folly::Unit{});
                    });
                },
                std::chrono::duration_cast<std::chrono::milliseconds>(duration)
            );
        }, nullptr /* no guard needed */);

        // 等待 future 完成
        co_await folly::coro::co_await(std::move(future));
    }

    // ── 一次性定时器 ──
    using TimerId = uint64_t;

    TimerId schedule_at(std::chrono::steady_clock::time_point deadline,
                        size_t target_worker_id,
                        folly::Function<void()> callback) {
        auto id = next_timer_id_.fetch_add(1);

        event_base_->runInEventBaseThread([this, deadline, target_worker_id,
                                            callback = std::move(callback), id]() {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (ms.count() <= 0) {
                // 已过期，立即执行
                executor_.add_to_worker(target_worker_id, std::move(callback));
            } else {
                wheel_timer_->scheduleTimeoutFn(
                    [this, target_worker_id, callback = std::move(callback)]() {
                        executor_.add_to_worker(target_worker_id, std::move(callback));
                    },
                    ms
                );
            }
        });

        return id;
    }

    // ── 周期定时器 ──
    TimerId schedule_periodic(std::chrono::nanoseconds interval,
                              size_t target_worker_id,
                              folly::Function<void()> callback) {
        auto id = next_timer_id_.fetch_add(1);

        // 使用递归调度实现周期定时器
        schedule_periodic_impl(id, interval, target_worker_id, std::move(callback));

        return id;
    }

private:
    void schedule_periodic_impl(TimerId id,
                                std::chrono::nanoseconds interval,
                                size_t target_worker_id,
                                folly::Function<void()> callback) {
        event_base_->runInEventBaseThread([this, id, interval, target_worker_id,
                                            callback = std::move(callback)]() mutable {
            if (cancelled_timers_.count(id)) return;

            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
            wheel_timer_->scheduleTimeoutFn(
                [this, id, interval, target_worker_id, callback = std::move(callback)]() mutable {
                    // 周期触发 → 通过 add_to_worker 路由
                    executor_.add_to_worker(target_worker_id, [callback = std::move(callback)]() {
                        callback();
                    });
                    // 递归注册下一轮
                    schedule_periodic_impl(id, interval, target_worker_id, std::move(callback));
                },
                ms
            );
        });
    }

    WorkStealingExecutor& executor_;
    std::unique_ptr<folly::EventBase> event_base_;
    folly::HHWheelTimer* wheel_timer_ = nullptr;  // 不拥有，由 EventBase 管理
    std::thread timer_thread_;
    std::atomic<uint64_t> next_timer_id_{0};
    std::unordered_set<TimerId> cancelled_timers_;
};
```

**线程模型**:
```
TimerScheduler 全局线程:
  └── EventBase loop (IoUringBackend)
      └── HHWheelTimer
          └── 定时器到期回调
              └── executor_.add_to_worker(target_worker_id, continuation)
                  └── WorkStealingExecutor 的 worker 线程执行

线程数: TimerScheduler 启动后 +1 个线程（全局定时器线程）
  这是唯一的新增线程，所有其他定时/事件组件复用此机制。
```

### 9. io_uring 集成路径

#### 9.1 I/O 线程的职责边界

**核心结论: I/O 线程是 io_uring 驱动器 + 事件分发器，不是数据的接收者/发送者。**

io_uring 模型下，数据收发由**内核**完成——内核通过 DMA 直接将数据写入用户态 buffer。I/O 线程不参与数据搬运，只做三件事：

```
I/O 线程 (EventBase + IoUringBackend) 的职责:
  ├── 提交 I/O 请求: 将 SQE 提交到 io_uring（read/write/connect/accept/fsync）
  ├── 监听完成事件: 处理 io_uring CQE（内核已完成 I/O）
  └── 投递 continuation: 调用 executor.add_to_worker() 通知协程

I/O 线程不做的事:
  ├── 不接收数据（recv 由内核异步完成，DMA 直写用户态 buffer）
  ├── 不发送数据（send 由内核异步完成）
  ├── 不解析数据（在 executor worker 上做）
  └── 不执行业务逻辑（在 executor worker 上做）
```

完整的收发数据流：

```
═══ 接收 (Read) ═══

  Worker 线程                           I/O 线程                    内核
  ──────────                            ──────                     ────
  co_await socket.read(buf)
    │→ buf 提交给 io_uring ────────→ SQE 入环
    │  协程挂起                          │
    │                                    │              ←── 内核异步执行 read
    │                                    │              数据已写入 buf
    │                          CQE 完成 ←── 通知完成 ──→
    │                                    │
    │  ← add_to_worker(w, continuation) ─┤
    │                                    │
  pop → resume 协程
  buf 中数据已就绪，直接处理
  (解析/计算/业务逻辑)

═══ 发送 (Write) ═══

  Worker 线程                           I/O 线程                    内核
  ──────────                            ──────                     ────
  准备好 data
  co_await socket.write(data)
    │→ data 提交给 io_uring ────────→ SQE 入环
    │  协程挂起                          │
    │                                    │              ←── 内核异步执行 write
    │                          CQE 完成 ←── 通知完成 ──→
    │                                    │
    │  ← add_to_worker(w, continuation) ─┤
    │
  pop → resume 协程
  write 完成，继续业务逻辑
```

**与传统 reactor 的对比**:

```
传统 reactor (epoll):
  I/O 线程: epoll_wait → recv(fd, buf) → 处理数据 → dispatch
            ↑ recv 是同步系统调用，在 I/O 线程上执行，数据拷贝也在此

io_uring reactor:
  I/O 线程: 提交 SQE → 处理 CQE → add_to_worker(continuation)
            ↑ 不执行 recv/read，内核异步完成，数据已就绪
```

#### 9.2 io_uring 的局限与全用户态栈的未来演进

**当前 io_uring 的本质**: I/O 请求由内核线程完成，数据收发全在内核态。上游（用户态）对 I/O 的调度、优先级、流控没有直接控制力，只能提交请求、等待完成。

这意味着：
- I/O 延迟 = 系统调用开销 + 内核调度 + 硬件延迟，用户态无法优化
- 无法实现"收→解析→执行→回复"的单线程流水线——收发在内核，解析/执行在 worker，必然跨线程
- 对于超低延迟场景（如高频交易），内核态 I/O 是瓶颈

**未来演进方向: 全用户态 I/O 栈**

当需要极致低延迟时，可切换到全用户态网络栈（如 DPDK/SPDK）：

```
全用户态栈模型（未来）:
  每个 Worker 线程独立完成:
    poll 网卡 → 收包 → 解析 → 执行业务 → 构造回复 → 发包
    └── 全在同一线程，无跨线程通信，延迟可控到亚微秒级

  与当前 io_uring 模型的区别:
  ┌──────────────┬──────────────────────┬────────────────────────┐
  │              │ io_uring (当前)       │ 全用户态 (未来)         │
  ├──────────────┼──────────────────────┼────────────────────────┤
  │ 数据收发     │ 内核线程完成          │ Worker 线程直接 poll    │
  │ 解析/执行    │ Worker 线程           │ 同一 Worker 线程        │
  │ 跨线程通信   │ I/O 线程→Worker 线程  │ 无（单线程流水线）      │
  │ 延迟         │ ~5-50μs              │ < 1μs                  │
  │ 依赖         │ Linux 5.1+           │ DPDK/SPDK + 独占网卡   │
  │ 适用场景     │ 通用量化              │ 高频交易                │
  └──────────────┴──────────────────────┴────────────────────────┘

  架构适配:
    WorkStealingExecutor 增加 UserSpaceIOMode:
      - io_uring 模式: 保持 I/O 线程 + worker 线程分离
      - 全用户态模式: Worker 线程在 worker_loop 中增加 poll 网卡步骤
        poll → 有数据 → 解析 → 执行 → 回复 → 继续任务调度
        poll → 无数据 → 继续正常的 LocalDeque/GlobalQueue/steal/park
```

**当前决策**: 先用 io_uring，架构预留全用户态演进能力。全用户态栈的实现需要独立网卡、大页内存、CPU 亲和绑核等，是独立的大工程，不在本次改造范围内。

#### 9.3 EventBase 后端配置

```cpp
// 方式（推荐）: WorkStealingExecutor 持有一个共享 EventBase
//   所有需要 I/O 的模块通过 executor 获取 EventBase
class WorkStealingExecutor : public folly::Executor {
    // ...
    folly::EventBase* io_event_base() const { return io_event_base_.get(); }

private:
    // I/O EventBase (IoUringBackend)
    std::unique_ptr<folly::EventBase> io_event_base_;
    std::thread io_thread_;  // I/O 事件循环线程
};
```

#### 9.4 I/O 操作如何与 WorkStealingExecutor 协作

```
DiskPersistence::co_write_segment(data):
  ┌─────────────────────────────────────────────────────────────┐
  │ 1. Worker 准备好 data，调用 co_await socket.write(data)     │
  │ 2. 底层将 write SQE 提交到 io_uring                         │
  │ 3. 协程挂起，记录 caller_worker_id                          │
  │                                                             │
  │ 4. 内核异步完成 write，DMA 直写磁盘                         │
  │ 5. I/O 线程处理 CQE 完成事件                                │
  │ 6. I/O 线程调用 executor_.add_to_worker(caller_worker_id,  │
  │    continuation)                                            │
  │ 7. Worker 从 LocalDeque pop → resume 协程                   │
  │ 8. Worker 在 resume 后继续业务逻辑                          │
  └─────────────────────────────────────────────────────────────┘
```

#### 9.5 网络 I/O: AsyncIoUringSocket

```
TcpConnection 协程化:
  ┌─────────────────────────────────────────────────────────────┐
  │ co_connect(host, port):                                     │
  │   auto socket = AsyncIoUringSocket::newSocket(              │
  │       executor.io_event_base());                            │
  │   co_await socket.connect(addr);                            │
  │   // connect 完成后，I/O 线程投递 CQE → worker resume       │
  │                                                             │
  │ co_send(data):                                              │
  │   co_await socket.write(data);                              │
  │   // Worker 准备 data → SQE → 内核发送 → CQE → worker resume│
  │                                                             │
  │ co_recv(buf, len):                                          │
  │   auto result = co_await socket.read(buf, len);             │
  │   // buf 提交给内核 → 内核 DMA 写入 → CQE → worker resume   │
  │   // Worker resume 后 buf 已就绪，直接解析处理              │
  │                                                             │
  │ 线程亲和性:                                                 │
  │   所有 I/O 完成回调在 io_thread_ 上触发                     │
  │   通过 add_to_worker(caller_worker_id, continuation) 路由   │
  │   Worker resume 后在原 worker 线程上继续执行                │
  └─────────────────────────────────────────────────────────────┘
```

### 10. StatRegistry 集成点全览

```
WorkStealingExecutor 内部:
┌────────────────────────────────────────────────────────────────┐
│ add()                → increment("executor.tasks_submitted")   │
│ 任务执行完成         → increment("executor.tasks_completed")   │
│ 协程恢复             → increment("executor.handles_resumed")   │
│ LocalDeque pop       → increment("executor.local_pops")        │
│ GlobalQueue pop      → increment("executor.global_queue_pops") │
│ 偷取成功             → increment("executor.local_steals")      │
│ 偷取失败             → increment("executor.failed_steals")     │
│ futex park           → increment("executor.park_count")        │
│ futex unpark         → increment("executor.unpark_count")      │
│ 任务耗时             → observe_timer("executor.task_latency_us")│
│ Worker 利用率        → set_gauge("executor.worker_N.util", %)  │
│ 队列深度             → set_gauge("executor.worker_N.queue_depth")│
└────────────────────────────────────────────────────────────────┘

TimerScheduler:
┌────────────────────────────────────────────────────────────────┐
│ 定时器注册          → increment("timer.scheduled")             │
│ 定时器到期          → increment("timer.fired")                 │
│ 定时器取消          → increment("timer.cancelled")             │
│ 调度延迟            → observe_timer("timer.fire_latency_us")   │
└────────────────────────────────────────────────────────────────┘

ExecutionDAG:
┌────────────────────────────────────────────────────────────────┐
│ 层级执行延迟        → observe_timer("dag.layer_N_latency_ms")  │
│ 任务执行            → increment("dag.tasks_executed")          │
│ 任务失败            → increment("dag.tasks_failed")            │
└────────────────────────────────────────────────────────────────┘

DiskPersistence:
┌────────────────────────────────────────────────────────────────┐
│ 写入延迟            → observe_timer("disk.write_latency_us")   │
│ 读取延迟            → observe_timer("disk.read_latency_us")    │
│ fsync 延迟          → observe_timer("disk.fsync_latency_us")   │
└────────────────────────────────────────────────────────────────┘

Network:
┌────────────────────────────────────────────────────────────────┐
│ 发送延迟            → observe_timer("net.send_latency_us")     │
│ 接收延迟            → observe_timer("net.recv_latency_us")     │
│ 连接延迟            → observe_timer("net.connect_latency_us")  │
└────────────────────────────────────────────────────────────────┘

FactorComputer / RiskEngine / EventBus / Logger / CronScheduler:
┌────────────────────────────────────────────────────────────────┐
│ 各模块见 coroutine_refactor_discussion.md 中的统计映射表       │
└────────────────────────────────────────────────────────────────┘
```

### 11. 七条设计约束的完整体现映射

| # | 设计约束 | 在 WorkStealingExecutor 中的体现 | 代码位置 |
|---|---------|-------------------------------|---------|
| 1 | I/O 全量 io_uring 化 | `io_event_base_` 使用 `IoUringBackend`；所有 I/O 通过 `AsyncIoUringSocket`；I/O 线程仅驱动 io_uring（提交 SQE/处理 CQE/投递 continuation），不做数据收发（内核 DMA 完成）；I/O 完成回调通过 `add_to_worker` 路由；未来可演进为全用户态栈（DPDK/SPDK） | `WorkStealingExecutor::io_event_base()`, `DiskPersistence::co_write_segment`, `TcpConnection::co_connect` |
| 2 | 慢降级、快恢复 | worker_loop 三级退避: PAUSE spin(10μs) → yield(3轮) → futex park；唤醒后重置计数器立即取任务 | `WorkStealingExecutor::worker_loop()` |
| 3 | 线程亲和性 | `add()` 从 pool worker 调用时 push 到当前 worker 的 LocalDeque；`add_to_worker(id, fn)` 定向投递；co_submit/coroutine resume 记录 caller_worker_id | `WorkStealingExecutor::add()`, `add_to_worker()` |
| 4 | 仅单机多线程 | 无 RemoteQueue、无共享内存 ring buffer、无跨进程偷窃；只有 Chase-Lev Deque[N] + GlobalMPMCQueue | `WorkStealingExecutor` 类定义 |
| 5 | 全局线程 + timer loop | TimerScheduler: 全局 timer_thread_ + HHWheelTimer (IoUringBackend)；到期后 `add_to_worker(last_worker_id, continuation)` | `TimerScheduler::co_sleep()`, `schedule_at()` |
| 6 | Baton 走 executor 路由 | AffinityBaton: 完整替代 folly::coro::Baton，多等待者侵入式链表，`post()` 不直接 resume，而是 `executor.add_to_worker(waiter_worker_id_, resume_func)`；提供 `post_direct()` 兼容无 executor 场景 | `AffinityBaton::post()`, `AffinityBaton::post_direct()` |
| 7 | 全局统计类 | 所有计数器/延迟通过 `StatRegistry::increment()` / `observe_timer()` / `set_gauge()` | 所有模块的统计埋点 |

### 12. 线程模型总览

```
进程启动后的线程组成:
┌─────────────────────────────────────────────────────────────┐
│ 主线程                                                       │
│   → 初始化、启动 executor、提交初始任务                       │
├─────────────────────────────────────────────────────────────┤
│ Worker 线程 × N (WorkStealingExecutor)                       │
│   → worker_loop: LocalDeque → GlobalQueue → steal → park   │
│   → 所有协程任务、DAG 执行、业务逻辑都在这些线程上           │
│   → 任何时刻线程数 = N，不增长                               │
├─────────────────────────────────────────────────────────────┤
│ Timer 线程 × 1 (TimerScheduler)                              │
│   → EventBase loop + HHWheelTimer                           │
│   → 定时器到期后投递到 executor worker，不执行业务逻辑       │
├─────────────────────────────────────────────────────────────┤
│ I/O 线程 × 1 (WorkStealingExecutor::io_thread_)              │
│   → EventBase loop + IoUringBackend                         │
│   → 职责: 提交 SQE、处理 CQE、投递 continuation             │
│   → 不做数据收发（内核 DMA 完成）、不解析、不执行业务逻辑    │
│   → 未来可演进为全用户态栈（DPDK/SPDK），                    │
│     Worker 线程直接 poll 网卡，实现单线程流水线              │
├─────────────────────────────────────────────────────────────┤
│ 总计: 1 (主) + N (worker) + 1 (timer) + 1 (I/O)            │
│   = N + 3 个线程                                            │
│                                                             │
│ 对比旧方案:                                                 │
│   旧: 1 (主) + N (ThreadPool) + 1 (CronScheduler)          │
│       + 1 (EventBus) + 1 (Logger) + 1 (ConfigManager)       │
│       + 1 (WebSocket accept) + ...                          │
│   新: N + 3，无论多少组件都不增长                            │
└─────────────────────────────────────────────────────────────┘
```

## 开发阶段

### Phase 1: 基础设施层（原语 + 执行器）

#### 1.1 引入 Folly

**文件**: `CMakeLists.txt`

- `FetchContent_Declare(folly ...)` 默认开启
- 编译选项: `FOLLY_BUILD_SHARED=OFF`, `FOLLY_USE_SIMDHASH=OFF`
- 链接: `Folly::folly`, `Folly::folly_coro`
- 验证: 编译运行一个最小 `folly::coro::Task<int>`

#### 1.2 新建 `coroutine.h` — Folly 别名

**文件**: `cpp/quant/infra/coroutine.h`（覆盖原文件）

```cpp
#pragma once
#include <folly/coro/Task.h>
#include <folly/coro/Mutex.h>
#include <folly/coro/Semaphore.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/CurrentExecutor.h>
#include "affinity_baton.h"  // AffinityBaton 完整替代 folly::coro::Baton

namespace quant::infra {
template<typename T = void>
using CoTask = folly::coro::Task<T>;

using CoBaton = AffinityBaton;  // 完整替代 folly::coro::Baton，多等待者 + executor 路由
using CoMutex = folly::coro::Mutex;
using CoSemaphore = folly::coro::Semaphore;
using AsyncScope = folly::coro::AsyncScope;

using folly::coro::collectAll;
using folly::coro::collectAllRange;
using folly::coro::sleep;
using folly::coro::co_withExecutor;
using folly::coro::co_currentExecutor;
}
```

**删除**: 原 `Baton`, `CoroutineMutex`, `CoTask<T>`, `CoTaskPromise` 全部删除

#### 1.3 实现 Chase-Lev Deque

**新建文件**: `cpp/quant/infra/chase_lev_deque.h`

Lock-free work-stealing deque:
- `push(T)` — Owner only, bottom 端入队, 无锁
- `pop()` — Owner only, bottom 端出队 (LIFO), 无锁
- `steal()` — Any thread, top 端偷取 (FIFO), 无锁
- 动态扩容: circular array 按 2 倍增长
- 内存序: push 用 `release`, pop/steal 用 `acquire`/`acq_rel`

#### 1.4 实现 WorkStealingExecutor

**新建文件**: `cpp/quant/infra/work_stealing_executor.h`, `.cc`

```cpp
class WorkStealingExecutor : public folly::Executor {
public:
    explicit WorkStealingExecutor(size_t num_workers,
                                   std::string_view name_prefix = "quant-ws");
    ~WorkStealingExecutor() override;

    // folly::Executor 接口 — Folly coro 自动调用
    void add(Func func) override;

    // 线程亲和投递 — 定向到指定 worker 的 LocalDeque
    void add_to_worker(size_t worker_id, Func func);

    // 协程提交
    template<typename F>
    folly::coro::Task<std::invoke_result_t<F>> co_submit(F&& func);

    // 生命周期
    void start();
    void stop();
    void force_stop();

    // 统计（使用 StatRegistry）
    struct Stats {
        uint64_t tasks_submitted;
        uint64_t tasks_completed;
        uint64_t local_pops;
        uint64_t global_queue_pops;
        uint64_t local_steals;
        uint64_t failed_steals;
        uint64_t handles_resumed;
        uint64_t park_count;
        uint64_t unpark_count;
        size_t active_workers;
        double utilization;  // busy_cycles / total_cycles
    };
    Stats stats() const;

    // Worker ID 查询（线程亲和性基础）
    static size_t current_worker_id(); // thread_local

private:
    void worker_loop(size_t worker_id);

    std::vector<std::unique_ptr<ChaseLevDeque<WorkItem>>> local_deques_;
    GlobalMPMCQueue<WorkItem> global_queue_;
    std::vector<std::thread> workers_;
};
```

**add(Func) 的调度决策（线程亲和性）**:
```
if (当前线程是本 pool 的 worker_id=W):
    push 到 local_deques_[W]     ← O(1), 无锁, cache 最优
else:
    push 到 GlobalQueue          ← 外部提交
```

**add_to_worker(worker_id, Func) 的调度决策**:
```
push 到 local_deques_[worker_id]
如果该 worker 正在 park → 唤醒它
```

**worker_loop 的三级 park（慢降级快恢复）**:
```
while (running) {
    1. pop 自己的 LocalDeque
    2. 从 GlobalQueue 取
    3. 随机偷其他 worker 的 Deque
    4. 全部空 → 三级退避:
       a. PAUSE 自旋 (spin_max_us, 默认 10μs)
       b. yield (yield_rounds, 默认 3 轮)
       c. futex park → 等待 add/add_to_worker 唤醒
}
```

**统计全部走 StatRegistry**:
```
add() 时:       StatRegistry::increment("executor.tasks_submitted")
执行完成时:     StatRegistry::increment("executor.tasks_completed")
偷取成功时:     StatRegistry::increment("executor.local_steals")
park 时:        StatRegistry::increment("executor.park_count")
任务耗时:       StatRegistry::observe_timer("executor.task_latency_us", elapsed_us)
```

**删除**: `thread_pool.h`, `thread_pool.cc` 被 `work_stealing_executor.h/.cc` 替代

#### 1.5 实现 AffinityBaton

**新建文件**: `cpp/quant/infra/affinity_baton.h`

解决 `folly::coro::Baton::post()` 直接 resume 不走 executor 的问题：

```cpp
class AffinityBaton {
public:
    bool ready() const noexcept;

    // 协程等待
    struct Awaiter { ... };
    Awaiter operator co_await() const noexcept;

    // post 时通过 executor 路由到 last_worker_id，不直接 resume
    void post(WorkStealingExecutor& executor);

    void reset() noexcept;

private:
    folly::coro::Baton baton_;
    // await_suspend 时记录 last_worker_id
    // post 时调用 executor.add_to_worker(last_worker_id, resume_func)
};
```

#### 1.6 单元测试 — Phase 1

**文件**: `cpp/test/chase_lev_deque_test.cc`

| 测试用例 | 验证目标 |
|---------|---------|
| `SinglePushPop` | push 后 pop 得到同一元素 |
| `FifoSteal` | push A, B → steal 得到 A (FIFO) |
| `LifoPop` | push A, B → pop 得到 B (LIFO) |
| `ConcurrentSteal` | 4 个 thief 同时 steal，无数据竞争，无重复 |
| `EmptyPopReturnsNullopt` | 空 deque pop 返回 nullopt |
| `EmptyStealReturnsNullopt` | 空 deque steal 返回 nullopt |
| `DynamicResize` | push 超过初始容量后自动扩容 |

**文件**: `cpp/test/work_stealing_executor_test.cc`

| 测试用例 | 验证目标 |
|---------|---------|
| `AddAndExecute` | add(func) 后 func 被执行 |
| `CoSubmitReturnsValue` | co_submit 返回正确结果 |
| `CoSubmitVoid` | co_submit void 任务正确完成 |
| `ExceptionPropagation` | co_submit 抛异常能被捕获 |
| `ParallelExecution` | 100 个 co_submit 全部完成 |
| **`NoThreadGrowth`** | 提交 1000 个协程任务，线程数不增长 |
| **`HighUtilization`** | 100 个任务在 4 个 worker 上运行，每个 worker 都有执行记录 |
| **`CoroutineSchedulingCorrectness`** | co_await co_submit 的协程在 pool worker 上恢复 |
| **`StealCountNonZero`** | 不均衡负载下，steal 次数 > 0 |
| **`ThreadAffinity`** | add() 在 worker 线程上调用时，任务入该 worker 的 LocalDeque，该 worker 下一轮 pop 拿到 |
| **`AddToWorker`** | add_to_worker(id, fn) 定向投递到指定 worker |
| **`SlowDegradeFastRecover`** | 空闲 worker 经历 spin→yield→futex 三级退避；任务到达后 futexWake 立即恢复 |
| `StatsViaStatRegistry` | 统计数据通过 StatRegistry 读取，数值正确 |

**文件**: `cpp/test/affinity_baton_test.cc`

| 测试用例 | 验证目标 |
|---------|---------|
| `PostResumesOnExecutor` | post(executor) 后协程在 executor worker 上恢复（不在 post 调用线程） |
| `PostResumesOnLastWorker` | post(executor) 后协程回到上次执行的 worker_id |
| `CrossThreadSync` | 线程 A co_await baton，线程 B post()，协程正确恢复 |
| `MultipleWaitersAllResumed` | 多个协程等待同一 baton，post 后全部恢复 |
| `MultipleWaitersThreadAffinity` | 多个协程在不同 worker 上等待，post 后各自回到原 worker |
| `PostDirectResumesInline` | post_direct() 直接 resume，不走 executor |
| `ReadyNoSuspend` | baton 已 ready 时 co_await 不挂起 |
| `ResetAndReuse` | reset 后 baton 可再次 co_await/post |
| `TryWaitReturnsCorrectState` | try_wait() 在 post 前后返回正确值 |

**文件**: `cpp/test/coroutine_test.cc`（重写）

| 测试用例 | 验证目标 |
|---------|---------|
| `CoTaskReturnValue` | CoTask<int> 返回正确值 |
| `CoTaskVoid` | CoTask<void> 正确完成 |
| `CoBatonWaitPost` | co_await CoBaton 后 post(executor) 恢复，回到原 worker |
| `CoMutexLockUnlock` | co_await mutex 加锁解锁 |
| `CollectAllParallel` | collectAll 多任务并行完成 |
| `SleepResumesOnExecutor` | co_await sleep 后协程回到 executor |
| `BlockingWaitBridge` | folly::coro::blockingWait 正确桥接同步/异步 |

---

### Phase 2: 编排层（统一 DAG）

#### 2.1 合并 TaskGraph + FactorDAG 为 ExecutionDAG

**新建文件**: `cpp/quant/scheduler/execution_dag.h`, `.cc`

```cpp
class ExecutionDAG {
public:
    using TaskFunc = std::function<folly::coro::Task<void>()>;

    TaskId add_task(std::string name, TaskFunc func);
    bool add_dependency(TaskId task, TaskId depends_on);
    GraphValidationResult validate() const;
    std::vector<std::vector<TaskId>> parallel_levels() const;

    // 协程执行 — 任务优先提交到当前线程
    folly::coro::Task<ExecutionResult> co_execute(WorkStealingExecutor& executor);

    // 同步桥接
    ExecutionResult execute(WorkStealingExecutor& executor);

private:
    folly::coro::Task<void> co_run_task(TaskId id);
};
```

**co_execute 中的线程亲和性**:
```
对每层 collectAll 提交子任务时:
  如果当前在 worker-W 上执行:
    子任务通过 add_to_worker(W, ...) 优先提交到当前 worker
  如果其他 worker 空闲且当前 worker 队列深:
    被 steal 走（负载均衡）
```

**统计**: 每层执行后 `StatRegistry::observe_timer("dag.layer_N_latency_ms", elapsed)`

**删除**: `factor_dag.h`, `factor_dag.cc`

#### 2.2 WaveScheduler 重写为薄层

**修改文件**: `cpp/quant/scheduler/wave_scheduler.h`, `.cc`

- `execute()` 内部委托给 `ExecutionDAG::co_execute()`
- 删除所有 `std::thread` 创建代码
- `max_concurrency` 改为 `CoSemaphore` 限流

#### 2.3 FactorComputer 协程化

**修改文件**: `cpp/quant/factor/factor_computer.h`, `.cc`

- `co_compute_all()`: 同层因子用 `collectAllRange` 并行
- `compute_all()`: 同步桥接 `blockingWait(co_compute_all())`
- `compute_factor_impl` 返回 `CoTask<ComputeResult>`

#### 2.4 单元测试 — Phase 2

**文件**: `cpp/test/execution_dag_test.cc`

| 测试用例 | 验证目标 |
|---------|---------|
| `EmptyDAG` | 空 DAG 返回成功 |
| `LinearChain` | A→B→C 按顺序执行 |
| `ParallelExecution` | 同层 4 个任务并行完成 |
| **`NoThreadCreation`** | DAG 执行不创建线程（全部在 executor worker 上） |
| **`DagLayerOrder`** | 第 N 层的任务在第 N-1 层全部完成后才开始 |
| **`ThreadAffinityOnSubmit`** | DAG 提交任务时，优先进入当前 worker 的 LocalDeque |
| `CycleDetection` | 有环的 DAG validate() 返回 false |
| `LargeDAG` | 100 节点 DAG 正确执行 |
| `FailedTask` | 任务抛异常，DAG 正确报告失败 |
| `StatsRecorded` | 执行后 StatRegistry 中有 dag 层级延迟数据 |

**文件**: `cpp/test/factor_test.cc`（更新）

| 测试用例 | 验证目标 |
|---------|---------|
| `CoComputeAllParallel` | 同层因子并行计算 |
| `CoComputeSingleFactor` | 单因子计算正确 |
| **`ParallelSpeedup`** | 4 个无依赖因子在 4 worker 上运行时间 ≈ 1 个因子时间 |
| `StatsInRegistry` | 因子计算延迟在 StatRegistry 中可查 |

---

### Phase 3: I/O 层（io_uring）

#### 3.1 IoUringBackend 集成

**修改文件**: `CMakeLists.txt`

- 添加 liburing 依赖
- 配置 IoUringBackend 为 EventBase 后端

#### 3.2 DiskPersistence 协程化

**修改文件**: `cpp/quant/storage/disk_persistence.h`, `.cc`

- `co_write_segment()`: 通过 IoUringBackend 的 `queueWrite` + `queueFsync` 异步写
- `co_read_segment()`: 通过 `queueRead` 异步读
- 同步接口保留，内部 `blockingWait`
- 统计: `StatRegistry::observe_timer("disk.write_latency_us", ...)`, `"disk.read_latency_us"`

#### 3.3 TcpConnection 协程化

**修改文件**: `cpp/quant/network/tcp_connection.h`, `.cc`

- 用 `AsyncIoUringSocket` 替换原生 POSIX socket
- `co_connect()`, `co_send()`, `co_recv()` 基于 AsyncIoUringSocket
- 统计: `StatRegistry::observe_timer("net.send_latency_us", ...)`, `"net.recv_latency_us"`

#### 3.4 WebSocket 协程化

**修改文件**: `cpp/quant/network/websocket_server.h`, `.cc`

- `co_accept_loop()`: 用 AsyncIoUringSocket 的异步 accept
- `co_handle_session()`: 每个连接一个协程处理收发
- 删除 `accept_thread`

#### 3.5 单元测试 — Phase 3

| 测试用例 | 验证目标 |
|---------|---------|
| `CoWriteReadRoundtrip` | co_write + co_read 数据一致 |
| `CoFsyncAfterWrite` | SyncMode::kSync 下 fsync 正确执行 |
| `CoConnectTimeout` | 协程连接超时正确处理 |
| **`NoBlockingOnIO`** | I/O 操作期间不阻塞 worker 线程 |
| `IoUringBackendEnabled` | EventBase 使用 IoUringBackend 而非 epoll |
| `IOLatencyInStats` | 磁盘/网络 I/O 延迟记录在 StatRegistry |

---

### Phase 4: 定时/事件组件协程化

#### 4.1 TimerScheduler

**新建文件**: `cpp/quant/infra/timer_scheduler.h`, `.cc`

```cpp
class TimerScheduler {
public:
    explicit TimerScheduler(WorkStealingExecutor& executor);

    // 全局线程 + HHWheelTimer
    void start();
    void stop();

    // 协程定时 — 到期后协程回到 last_worker_id
    folly::coro::Task<void> co_sleep(std::chrono::duration d);

    // 一次性定时器
    template<typename F>
    TimerId schedule_at(time_point tp, size_t target_worker_id, F&& func);

    // 周期定时器
    template<typename F>
    TimerId schedule_periodic(duration interval, size_t target_worker_id, F&& func);

private:
    WorkStealingExecutor& executor_;
    folly::EventBase event_base_;  // 使用 IoUringBackend
    std::thread timer_thread_;
    // last_worker_id 存储在定时器回调的闭包中
};
```

到期后的路由:
```
定时器到期:
  1. HHWheelTimer callback 触发
  2. 读取闭包中保存的 last_worker_id
  3. 调用 executor_.add_to_worker(last_worker_id, continuation)
  4. 协程在 last_worker_id 的 worker 上恢复
```

#### 4.2 CronScheduler 协程化

**修改文件**: `cpp/quant/scheduler/cron_scheduler.h`, `.cc`

- 删除专用线程 `scheduler_thread_`
- 使用 `TimerScheduler::co_sleep` 等待下一个触发时间
- `start()`: 在 executor 上启动 `co_tick_loop()`
- 统计: `StatRegistry::observe_timer("cron.tick_latency_ms", ...)`

#### 4.3 EventBus 协程化

**修改文件**: `cpp/quant/event/event_bus.h`, `.cc`

- 删除专用线程 `async_worker_`
- `co_dispatch_loop()`: 用 `AffinityBaton` 等待新事件
- `publish_async()`: 入队后 `affinity_baton_.post(executor_)`
- 统计: `StatRegistry::increment("eventbus.published")`, `observe_timer("eventbus.dispatch_latency_us", ...)`

#### 4.4 Logger 协程化

**修改文件**: `cpp/quant/infra/logging/logger.cc`

- 删除专用线程 `flush_thread_`
- `co_flush_loop()`: 用 `AffinityBaton` 等待入队通知
- 入队后 `flush_baton_.post(executor_)`
- 统计: `StatRegistry::increment("logger.records_written")`, `"logger.records_dropped"`

#### 4.5 ConfigManager 协程化

**修改文件**: `cpp/quant/infra/config/config_manager.cc`

- 删除专用线程 `hot_reload_thread_`
- 使用 `TimerScheduler::co_sleep(poll_interval)` 替代 `sleep_for`

#### 4.6 单元测试 — Phase 4

| 测试用例 | 验证目标 |
|---------|---------|
| **`TimerSchedulerThreadAffinity`** | 定时器到期后协程回到 last_worker_id 的 worker |
| `CoSleepResumesOnExecutor` | co_sleep 后协程在 executor worker 上恢复 |
| `CronNoDedicatedThread` | CronScheduler start() 后不增加线程数 |
| `CronPreciseFire` | 协程版 tick 在精确时间触发 |
| `EventBusNoDedicatedThread` | EventBus start() 后不增加线程数 |
| `AffinityBatonPostRoute` | post 后协程通过 executor 路由恢复 |
| `LoggerNoDedicatedThread` | Logger 不创建专用线程 |
| `SlowDegradeOnIdle` | 无任务时 worker 经历 spin→yield→futex 降级 |
| `FastRecoverOnTask` | futex park 的 worker 在任务到达后 < 1μs 唤醒 |

---

### Phase 5: 热路径锁协程化

#### 5.1 AlgorithmicTrader

`std::mutex` → `CoMutex`, `on_tick` → `co_on_tick`

#### 5.2 RiskEngine

`std::mutex` → `CoMutex`, `check` → `co_check`（并行规则检查用 collectAll）

#### 5.3 OrderManager

同上

---

## 测试观测体系

### 核心观测指标

#### 1) 协程调度是否符合预期

**观测方法**: 在 WorkStealingExecutor 中埋点，记录每个调度事件

```cpp
struct SchedulingEvent {
    std::string type;          // "submit" | "pop" | "steal" | "resume" | "park" | "unpark"
    size_t worker_id;
    uint64_t timestamp_ns;
    size_t target_worker_id;   // 仅 add_to_worker 时有值
};
```

**验证规则**:

| 规则 | 验证方式 |
|------|---------|
| co_await co_submit(f) 的调用者恢复在 executor worker 上 | 记录 resume 时的 thread_id，验证是 pool worker |
| co_await sleep(d) 后回到 last_worker_id | 记录 sleep 前后 worker_id，验证相同 |
| collectAll 中的子任务并行执行 | 记录各子任务的 worker_id，验证不全是同一个 |
| DAG 层级间有 happens-before 关系 | 记录各层级任务的开始/结束时间，验证无交叉 |
| AffinityBaton post 后协程在 executor worker 上恢复 | 记录恢复时 worker_id != post 调用者的线程 |

#### 2) 没有线程增长

**观测方法**:

```cpp
auto count_threads = []() {
    // 读取 /proc/self/status 的 Threads: 行
};
size_t before = count_threads();
// ... 运行 1000 个协程任务 ...
size_t after = count_threads();
ASSERT_EQ(after, before + executor.worker_count());  // 只有 executor 的 worker 线程
```

**验证规则**:

| 场景 | 线程数预期 |
|------|-----------|
| 启动前 | N (主线程等) |
| Executor 启动后 | N + worker_count |
| 提交 1000 个 co_submit | 不增长 |
| 启动 TimerScheduler | +1 (全局定时器线程) |
| 启动 CronScheduler | 不增长（不再创建专用线程） |
| 启动 EventBus | 不增长 |
| 启动 Logger | 不增长 |
| 执行 100 层 DAG | 不增长 |
| I/O 操作 | 不增长 |

#### 3) 线程池利用率始终在高位

**观测方法**: WorkStealingExecutor 内部维护 busy/idle 周期计数

```cpp
// worker_loop 中:
auto task_start = steady_clock::now();
execute(task);
auto task_end = steady_clock::now();
busy_cycles += (task_end - task_start).count();

// 采样接口
double utilization = busy_cycles.load() / (busy_cycles.load() + idle_cycles.load());
```

**验证规则**:

| 场景 | 利用率预期 |
|------|-----------|
| 4 worker 跑 100 个 co_submit | 所有 worker 利用率 > 80% |
| DAG 执行中（有并行层） | 利用率与并行度匹配 |
| 只有 1 个任务 | 1 个 worker ~100%，其余 ~0% |
| 空闲 | 所有 worker ~0% |
| 偷窃活跃时 | 被偷的 worker 利用率下降，偷取者利用率上升 |

### 集成验证测试

**文件**: `cpp/test/coroutine_integration_test.cc`

```cpp
// 端到端: 数据拉取 → 因子计算 → 风控 → 下单
folly::coro::Task<void> full_pipeline(WorkStealingExecutor& executor) {
    ExecutionDAG dag;
    auto data_id   = dag.add_task("data_pull", co_data_pull);
    auto factor_id = dag.add_task("factor_compute", co_factor_compute);
    auto risk_id   = dag.add_task("risk_check", co_risk_check);
    auto order_id  = dag.add_task("order_execute", co_order_execute);
    dag.add_dependency(factor_id, data_id);
    dag.add_dependency(risk_id, factor_id);
    dag.add_dependency(order_id, risk_id);

    auto result = co_await dag.co_execute(executor);
    EXPECT_TRUE(result.success);
}

TEST(Integration, NoThreadGrowth) {
    size_t t0 = count_threads();
    WorkStealingExecutor executor(4);
    executor.start();
    size_t t1 = count_threads();
    EXPECT_EQ(t1 - t0, 4);  // 只有 4 个 worker 线程

    folly::coro::blockingWait(full_pipeline(executor));

    size_t t2 = count_threads();
    EXPECT_EQ(t2, t1);  // 管道执行后无增长
}

TEST(Integration, HighUtilization) {
    WorkStealingExecutor executor(4);
    executor.start();

    // 采样利用率
    std::vector<double> samples;
    std::atomic<bool> done{false};
    std::thread sampler([&] {
        while (!done) {
            samples.push_back(executor.stats().utilization);
            std::this_thread::sleep_for(1ms);
        }
    });

    folly::coro::blockingWait(full_pipeline(executor));
    done = true;
    sampler.join();

    // 管道执行期间峰值利用率 > 50%
    EXPECT_GT(*std::max_element(samples.begin(), samples.end()), 0.5);
}

TEST(Integration, ThreadAffinityOnTimer) {
    WorkStealingExecutor executor(4);
    executor.start();
    TimerScheduler timer(executor);

    // 记录 co_sleep 前后的 worker_id
    size_t before_worker = WorkStealingExecutor::current_worker_id();
    folly::coro::blockingWait(co_withExecutor(executor, [&]() -> CoTask<void> {
        co_await timer.co_sleep(10ms);
    }));
    size_t after_worker = WorkStealingExecutor::current_worker_id();

    // 协程应回到同一个 worker
    EXPECT_EQ(before_worker, after_worker);
}
```

---

## 文件改动总览

### 删除

```
cpp/quant/infra/coroutine.h              (自写原语，被 Folly 别名替代)
cpp/quant/infra/thread_pool.h            (被 WorkStealingExecutor 替代)
cpp/quant/infra/thread_pool.cc
cpp/quant/factor/factor_dag.h            (并入 ExecutionDAG)
cpp/quant/factor/factor_dag.cc
cpp/test/thread_pool_test.cc             (被 executor_test 替代)
```

### 新建

```
cpp/quant/infra/coroutine.h              (Folly 别名)
cpp/quant/infra/chase_lev_deque.h        (Lock-free work-stealing deque)
cpp/quant/infra/work_stealing_executor.h (implements folly::Executor)
cpp/quant/infra/work_stealing_executor.cc
cpp/quant/infra/affinity_baton.h         (Baton + executor 路由)
cpp/quant/infra/timer_scheduler.h        (全局线程 + HHWheelTimer + 亲和性)
cpp/quant/infra/timer_scheduler.cc
cpp/quant/scheduler/execution_dag.h      (统一 DAG)
cpp/quant/scheduler/execution_dag.cc
cpp/test/chase_lev_deque_test.cc
cpp/test/work_stealing_executor_test.cc
cpp/test/affinity_baton_test.cc
cpp/test/execution_dag_test.cc
cpp/test/coroutine_integration_test.cc
```

### 修改

```
CMakeLists.txt                           (FetchContent Folly + liburing)
cpp/quant/scheduler/wave_scheduler.h/.cc (co_execute, 删 std::thread)
cpp/quant/scheduler/cron_scheduler.h/.cc (TimerScheduler + co_await co_sleep)
cpp/quant/scheduler/scheduler_service.h/.cc (组装业务链路 DAG)
cpp/quant/event/event_bus.h/.cc          (co_dispatch_loop, AffinityBaton)
cpp/quant/factor/factor_computer.h/.cc   (co_compute_all, collectAll)
cpp/quant/infra/logging/logger.cc        (co_flush_loop, AffinityBaton)
cpp/quant/infra/config/config_manager.cc (TimerScheduler::co_sleep)
cpp/quant/storage/disk_persistence.h/.cc (co_write/read, io_uring)
cpp/quant/network/tcp_connection.h/.cc   (AsyncIoUringSocket, co_send/recv)
cpp/quant/network/websocket_server.h/.cc (co_accept, co_handle_session)
cpp/quant/execution/algorithmic_trader.h/.cc (CoMutex, co_on_tick)
cpp/quant/risk/risk_engine.h/.cc         (CoMutex, co_check)
cpp/test/coroutine_test.cc               (重写为 Folly 版)
cpp/test/scheduler_test.cc               (更新为协程版)
```

---

## 执行顺序与依赖

```
Phase 1 (基础设施) ← 所有后续 Phase 的前提
  1.1 CMake + Folly + liburing
  1.2 coroutine.h (Folly 别名)
  1.3 Chase-Lev Deque
  1.4 WorkStealingExecutor (含线程亲和 + 三级 park)
  1.5 AffinityBaton
  1.6 测试

Phase 2 (编排层) ← 依赖 Phase 1 的 executor
  2.1 ExecutionDAG
  2.2 WaveScheduler 重写
  2.3 FactorComputer 协程化
  2.4 测试

Phase 3 (I/O) ← 依赖 Phase 1 的 executor + IoUringBackend
  3.1 IoUringBackend 集成
  3.2 DiskPersistence
  3.3 TcpConnection
  3.4 WebSocket
  3.5 测试

Phase 4 (定时/事件) ← 依赖 Phase 1 的 TimerScheduler + AffinityBaton
  4.1 TimerScheduler
  4.2 CronScheduler
  4.3 EventBus
  4.4 Logger
  4.5 ConfigManager
  4.6 测试

Phase 5 (热路径锁) ← 依赖 Phase 1 的 CoMutex
  5.1 AlgorithmicTrader
  5.2 RiskEngine
  5.3 OrderManager

Phase 2 和 Phase 3 可以并行开发。
Phase 4 和 Phase 5 可以并行开发。
Phase 3 和 Phase 4 依赖 Phase 1 但互不依赖。
```
