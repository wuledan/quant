# 协程化改造 — 完整讨论记录

> 日期: 2026-05-18
> 背景: 用户审查系统后提出架构问题，经多轮讨论形成本记录
> 配套文档: `coroutine_refactor_dev_plan.md`（开发及测试规划）

## 一、问题溯源

### 问题 1: 协程化改造未落地
- `design_coroutine_refactor.md` 规划了 Folly 协程改造，但未实施
- `TaskAwaiter::await_suspend` 仍在 spawn detached thread（每 co_await 创建一个线程）
- 上次实现的 `coroutine.h` 中 `Baton`/`CoroutineMutex` 的 `await_suspend` 内部调用 `cv_.wait()`，本质是阻塞线程，属于"伪协程"

### 问题 2: WebSocket 架构澄清
- WebSocket 仅用于前端实时推送（行情/持仓/风控/策略/回测/订单）
- 外部数据通过 HTTP API (akshare) 获取，不走 WebSocket
- 已记录在 `docs/architecture_websocket.md`

### 问题 3: 线程本地统计
- 已实现 `StatRegistry` with `thread_local` `LocalHolder`
- 跨线程 `aggregate()` 和 `reset()` 可用
- 在 `cpp/quant/infra/thread_local_stats.h`

### 问题 4: Folly 引入决策
- 自写的 Baton/CoroutineMutex/CoTask 不可用（cv_.wait 阻塞线程）
- 必须引入 Folly coro 原语: `folly::coro::Task`, `Baton`, `Mutex`, `Semaphore`, `collectAll`, `sleep`
- 编译时间可接受，默认开启 Folly

### 问题 5: 调度机制
- Folly 没有 work-stealing 实现
- Folly 的 `CPUThreadPoolExecutor` 用共享 MPMC 队列 + LifoSem
- 需要自建 `WorkStealingExecutor` 实现 `folly::Executor` 接口
- Folly coro 通过 `executor_->add(continuation)` 恢复协程，任何 `folly::Executor` 实现均可

### 问题 6: DAG 编排 vs 协程调度
- **这两个是独立的层级**
- 编排层 (Orchestration): 依赖解析、拓扑排序、并行层级计算
- 调度层 (Scheduling): 线程分配、协程恢复、负载均衡、work-stealing
- 当前 `WaveScheduler` 把两层耦合在一起（既做编排又创建线程）
- 改造后: `ExecutionDAG` 只做编排，`WorkStealingExecutor` 只做调度

### 问题 7: 七条架构意见（第二轮讨论）

用户在审阅初版规划后提出七条具体意见，经调查后形成以下结论：

#### 7.1 I/O 操作非阻塞化，优先使用 io_uring

**调查结论**: Folly 已有完整的 io_uring 集成。

- `folly/io/async/IoUringBackend.h` — 作为 EventBase 后端实现，可直接替换 epoll
- `folly/io/async/AsyncIoUringSocket.h` — 基于 io_uring 的异步 Socket
- 支持操作: read/write/readv/writev, sendmsg/recvmsg, fsync/fdatasync, openat/close/statx, 零拷贝
- 与协程的关系: IoUringBackend 作为 EventBase 后端，完成事件通过 EventBase 调度机制唤醒等待协程
- 通过 `EventBase::Options().setBackendFactory()` 切换后端

**决策**: 所有 I/O（磁盘 + 网络）走 io_uring，用 Folly 的 `IoUringBackend` + `AsyncIoUringSocket`。

#### 7.2 线程调度器无任务时 CPU suspend，遵循慢降级、快恢复

**调查结论**: Folly 的 `SaturatingSemaphore` 已实现三级退避：

```
空闲降级路径（慢降级）:
  1. PAUSE 自旋 (~10μs)     — asm volatile("pause"), 对超线程友好
  2. yield                   — std::this_thread::yield(), 让出时间片
  3. futex 阻塞              — futex(FUTEX_WAIT), 内核调度睡眠

唤醒路径（快恢复）:
  post() → SaturatingSemaphore::post()
    → CAS NOTREADY → READY
    → 如果状态是 BLOCKED → futexWake() 立即唤醒
    → 被唤醒线程从 futex 返回 → 直接取任务执行
```

这正是"慢降级、快恢复"：从 spin→yield→futex 逐级让出资源（降级慢，~10μs+yield+阻塞），但任务到达时一次 futexWake 就立即恢复。

**决策**: `WorkStealingExecutor` 的 worker park 机制采用同样的三级退避，复用 Folly 的 `SaturatingSemaphore` 或自行实现等价逻辑。参数可调：
- `spin_max_us` (默认 10): 自旋上限
- `yield_rounds` (默认 3): yield 轮数
- 超过后进入 futex park

#### 7.3 DAG 层提交任务时优先提交到当前线程

**调查结论**: Folly 有 executor 亲和性，但没有线程亲和性。

- `BasePromise::await_transform` 对每个 `co_await` 自动注入 `co_viaIfAsync(executor_, ...)`
- 协程恢复时调用 `executor_->add(continuation)`，回到同一个 Executor 实例
- 但 `add()` 将任务放入共享队列，可能被任意 worker 取走——**没有线程亲和性**

**决策**: `WorkStealingExecutor` 实现线程亲和调度：

```
add(Func) 的决策逻辑:
  if (当前线程是本 pool 的 worker_id=W):
      push 到 local_deques_[W]      ← O(1), 无锁, cache 最优
  else:
      push 到 GlobalQueue           ← 外部提交

这样:
- 协程恢复时 executor_->add() 被调用
- 如果恢复发生在一个 pool worker 上（通常如此）→ 任务回到该 worker 的本地队列
- 该 worker 下一轮 pop 时立即拿到，L1/L2 cache 命中
- 如果该 worker 忙，其他 worker 可以 steal 走（负载均衡）
```

这是"优先本地，允许偷走"策略，兼顾亲和性和负载均衡。

#### 7.4 暂只考虑单机多线程，不考虑多进程

**决策**: 删除 Remote Stealing 相关设计。`WorkStealingExecutor` 只包含：
- Chase-Lev Deque[N] (per-worker local deque)
- GlobalMPMCQueue (外部提交和溢出)
- Local stealing (偷其他 worker 的 deque)

不实现共享内存 ring buffer、不实现跨进程偷窃。未来如需扩展，可在此架构上添加 RemoteQueue 层。

#### 7.5 定时器使用全局线程 + timer loop，触发后协程回到上一次执行线程

**调查结论**: Folly 的 `co_await sleep()` 使用 `ThreadWheelTimekeeper`（全局线程 + HHWheelTimer），定时器到期后通过 `ViaCoroutine` 把 continuation 投递回原 Executor——这保证了 executor 亲和性，但不保证线程亲和性。

Folly 的 `TaskPromiseBase` 存储 `Executor::KeepAlive<> executor_`，没有存储 `worker_id`。

**决策**: 自建 `TimerScheduler`，在 Folly HHWheelTimer 基础上增加线程亲和性：

```
TimerScheduler 设计:
├── 全局线程 + HHWheelTimer (复用 Folly)
├── 定时器到期后:
│   1. 查找注册该定时器的协程的 last_worker_id
│   2. 调用 executor_->add_to_worker(last_worker_id, continuation)
│      如果 last_worker_id 的队列不空（worker 活跃）→ 直接入队
│      如果 last_worker_id 的队列为空（worker 空闲）→ 先唤醒该 worker
│   3. 如果 last_worker_id 已经 stop → 走 GlobalQueue
└── 协程的 last_worker_id 存储方式:
    在 WorkStealingExecutor 的 thread_local 中维护:
    thread_local size_t current_worker_id;
    每次 worker_loop 执行任务时设置
    协程 await_resume 时可读取
```

`add_to_worker(worker_id, func)` 是 `WorkStealingExecutor` 的新接口，支持定向投递到指定 worker 的本地队列。

#### 7.6 协程间同步（Baton 跨线程）确认

**调查结论**: `folly::coro::Baton` 支持跨线程同步，但有重要细节：

- `post()` **直接在调用线程上 resume 等待的协程**，不走 `executor_->add()`
- 协程 A 在 worker-1 上 `co_await baton`，协程 B 在 worker-2 上 `baton.post()` → 协程 A 在 **worker-2** 上恢复
- 这绕过了 `ViaIfAsync` 的 executor 路由机制

**影响**:
- 优点: 跨线程同步本身是正确的，无锁（原子 CAS 链表），多等待者都能唤醒
- 问题: 恢复后协程不在自己的 executor 上，TLS/NUMA 状态可能不匹配

**决策**: 自建 `AffinityBaton`，完整替代 `folly::coro::Baton`，具备其全部能力：

- 多等待者: 侵入式链表（WaiterNode），每个节点记录 worker_id
- `post(executor)`: 遍历链表，每个等待者通过 `executor.add_to_worker(worker_id, continuation)` 路由回原 worker
- `post_direct()`: 不走 executor，直接 resume（兼容场景：析构、测试）
- API 1:1 对齐 folly::coro::Baton: ready() / try_wait() / reset() / co_await / post

**为什么不包装 folly::coro::Baton**: `Baton::post()` 内部的 `resume()` 调用无法拦截，必须自建等待链。

详细实现见 `coroutine_refactor_dev_plan.md` 第 7 节。

#### 7.7 统计使用封装好的全局统计类

**决策**: 所有组件的性能计数、延迟统计、错误计数等，统一使用 `StatRegistry`（`cpp/quant/infra/thread_local_stats.h`）。

具体映射:

| 组件 | 统计项 | StatRegistry key |
|------|--------|-----------------|
| WorkStealingExecutor | 任务提交/完成数 | `executor.tasks_submitted`, `executor.tasks_completed` |
| WorkStealingExecutor | 偷窃次数 | `executor.local_steals`, `executor.failed_steals` |
| WorkStealingExecutor | worker park/unpark | `executor.park_count`, `executor.unpark_count` |
| ExecutionDAG | 层级执行延迟 | `dag.layer_N_latency_ms` |
| FactorComputer | 因子计算耗时 | `factor.compute_latency_ms` |
| RiskEngine | 风控检查耗时 | `risk.check_latency_us` |
| Logger | 日志写入量 | `logger.records_written`, `logger.records_dropped` |
| EventBus | 事件投递量 | `eventbus.published`, `eventbus.delivered` |
| DiskPersistence | I/O 延迟 | `disk.write_latency_us`, `disk.read_latency_us` |

不再各模块自建 `std::atomic<uint64_t>` 计数器，全部走 `StatRegistry::increment()` / `observe_timer()`。

## 二、系统阻塞点全览

### P0 — 协程原语不可用（阻塞整个改造）

| 文件 | 问题 | 影响 |
|------|------|------|
| `infra/coroutine.h` Baton | `await_suspend` 内 `cv_.wait()` 阻塞线程 | 所有 co_await Baton 的协程都会阻塞 |
| `infra/coroutine.h` CoroutineMutex | 内部用 std::mutex 保护 waiter 列表 | 高竞争时阻塞 |
| `infra/thread_pool.h` PoolAwareAwaiter | 手动管理 CoTaskState，非 Folly 标准 | 与 Folly coro 生态不兼容 |

### P1 — 线程创建/阻塞（性能灾难）

| 文件 | 问题 | 影响 |
|------|------|------|
| `scheduler/wave_scheduler.cc:32-78` | 每波 spawn `std::thread` + join | 1000 任务 = 数百次线程创建/销毁 |
| `scheduler/cron_scheduler.cc:43-48` | 专用线程 + `sleep_for` 轮询 | 占 1 线程空转 |
| `event/event_bus.cc:120-143` | 专用线程 + `cv_.wait()` | 单线程消费，无法协程衔接 |
| `infra/logging/logger.cc:99-108` | 专用线程 + `cv_.wait_for(1ms)` | 1ms 空转唤醒 |
| `infra/config/config_manager.cc:99` | 专用线程 + `sleep_for` 轮询 | 热重载空转 |

### P2 — 缺少并行

| 文件 | 问题 | 影响 |
|------|------|------|
| `factor/factor_computer.cc:19-52` | `compute_all` 串行执行 topological_sort | 同层无依赖因子应并行 |

### P3 — 同步 I/O（需 io_uring 化）

| 文件 | 问题 | 影响 |
|------|------|------|
| `storage/disk_persistence.cc` | 同步 `ofstream::write` + `fsync` | 磁盘 I/O 阻塞调用线程 |
| `network/tcp_connection.cc` | 同步 `poll` + `send`/`recv` | 网络 I/O 阻塞 |
| `network/websocket_server.cc` | accept 循环未实现 | 需用 AsyncIoUringSocket |

### P4 — 热路径锁

| 文件 | 问题 | 影响 |
|------|------|------|
| `execution/algorithmic_trader.cc` | `std::mutex` on_tick | 热路径锁 |
| `risk/risk_engine.cc` | `std::mutex` on check | 风控热路径 |

## 三、架构决策

### 3.1 删除所有自写协程原语

删除 `coroutine.h` 中的 Baton、CoroutineMutex、CoTask。删除 `thread_pool.h` 中的 PoolAwareAwaiter 和 CoTaskState。用 `AffinityBaton`（完整替代 `folly::coro::Baton`，多等待者 + executor 路由）替代自写 Baton。

### 3.2 自建 WorkStealingExecutor (implements folly::Executor)

Folly 没有 work-stealing。自建实现，仅单机多线程：

```
WorkStealingExecutor
├── Chase-Lev Deque[N]     — 每个 worker 一个 lock-free deque
├── GlobalMPMCQueue        — 外部提交和溢出
├── folly::Executor::add() — Folly coro 自动调用此方法恢复协程
├── add_to_worker(id, fn)  — 定向投递（支持线程亲和性）
└── 三级 park 退避          — spin → yield → futex (慢降级快恢复)
```

Worker 获取任务顺序:
1. pop 自己的 LocalDeque (O(1), 无锁)
2. 从 GlobalQueue 取 (轻度竞争)
3. 随机偷其他 worker 的 Deque (只偷 top, FIFO)
4. 全部空 → 三级退避 park

### 3.3 I/O 全量 io_uring 化

- 磁盘 I/O: `co_write_segment` / `co_read_segment` 底层用 `IoUringBackend` 的 `queueWrite` / `queueRead` / `queueFsync`
- 网络 I/O: `co_connect` / `co_send` / `co_recv` 用 `AsyncIoUringSocket`
- WebSocket: accept 循环用 `AsyncIoUringSocket` 的 `co_accept`
- 配置: 通过 `EventBase::Options().setBackendFactory(IoUringBackend)` 切换

**I/O 线程职责**: I/O 线程是 io_uring 驱动器 + 事件分发器，不做数据收发。数据收发由内核异步完成（DMA 直写用户态 buffer），I/O 线程只提交 SQE、处理 CQE、投递 continuation 到 executor。所有数据解析和业务逻辑在 executor worker 上执行。

**io_uring 局限与未来演进**: io_uring 模式下数据收发由内核线程完成，上游对 I/O 调度/优先级/流控无直接控制力。未来如需极致低延迟，可切换到全用户态 I/O 栈（DPDK/SPDK），实现"收→解析→执行→回复"单线程流水线，延迟可控到亚微秒级。当前先用 io_uring，架构预留演进能力。

### 3.4 线程亲和性

- `add(Func)`: 如果当前线程是 pool worker，push 到该 worker 的 LocalDeque
- `add_to_worker(worker_id, Func)`: 定向投递到指定 worker 的 LocalDeque
- 定时器到期: 协程回到 `last_worker_id` 的 LocalDeque
- Baton post: 通过 `add_to_worker` 路由，不直接 resume
- AffinityBaton: 完整替代 folly::coro::Baton，多等待者侵入式链表，每个节点记录 worker_id，post 时各等待者通过 add_to_worker 路由回原 worker

### 3.5 统一 DAG 编排

合并 `TaskGraph` + `FactorDAG` 为 `ExecutionDAG`:
- 只做编排: 依赖声明、拓扑排序、parallel_levels()
- 不做调度: 不知道线程、不知道协程
- `TaskNode::execute_fn` 改为返回 `folly::coro::Task<void>` 的工厂

### 3.6 全局统计

所有性能计数统一使用 `StatRegistry`，不再各模块自建 atomic 计数器。

## 四、Folly 调度机制调查

### Folly 自带的 Executor

| Executor | 调度模型 | Work-Stealing |
|----------|---------|---------------|
| `CPUThreadPoolExecutor` | 共享 `UMPMCQueue` + `LifoSem` 唤醒 | **无** |
| `IOThreadPoolExecutor` | 每线程一个 `EventBase` + `NotificationQueue`，round-robin | **无** |
| `EDFThreadPoolExecutor` | 最早截止时间优先 | **无** |
| `ThreadedExecutor` | 每任务一个线程 | **无** |

**结论：Folly 完全没有 work-stealing 实现。** 它选了共享队列 + LIFO 唤醒（让少数线程热跑，其余深度休眠），而不是 per-worker deque + 互相偷。

### Folly coro::Task 与 Executor 的集成方式

1. **`co_withExecutor(executor, task)`** — 绑定 Task 到指定 Executor
2. **`co_viaIfAsync`** — 每个 `co_await` 被隐式变换，确保恢复回到原 Executor
3. **恢复流程**: `ViaCoroutine` 调用 `executor_->add(continuation)`

**有 executor 亲和性，无线程亲和性。** 协程回到同一个 Executor 实例，但可能在不同 worker 线程上恢复。需要自建线程亲和性。

### folly::coro::Baton 跨线程同步

- 支持: 协程 A 在线程 1 `co_await baton`，协程 B 在线程 2 `baton.post()`
- **关键细节**: `post()` 直接在调用线程上 `resume()` 等待协程，**不走 executor_->add()**
- 协程 A 会在**线程 2**（post 的调用线程）上恢复，绕过了 ViaIfAsync 的 executor 路由
- 无锁实现: 原子 CAS 链表，多等待者都能唤醒

### Folly io_uring 集成

- `IoUringBackend` — EventBase 的 io_uring 后端
- `AsyncIoUringSocket` — 基于 io_uring 的异步 Socket
- 支持: read/write/readv/writev, sendmsg/recvmsg, fsync, openat/close, 零拷贝, SQPOLL
- 通过 `EventBase::Options().setBackendFactory()` 切换

### Folly CPU park 机制

三级退避: PAUSE 自旋(~10μs) → yield → futex 阻塞
- 慢降级: 逐级让出 CPU 资源
- 快恢复: 任务到达 → futexWake → 被唤醒线程立即取任务执行

## 五、WorkStealingExecutor 内部设计（七条约束的完整体现）

> 详见 `coroutine_refactor_dev_plan.md` 中的同名章节，包含:
> - 核心数据结构: WorkItem、WorkerState、GlobalMPMCQueue
> - 线程身份机制: thread_local worker_id 的设置与读取
> - add()/add_to_worker() 的完整调度决策逻辑
> - Worker Loop 三级退避完整伪代码（spin→yield→futex）
> - Folly coro::Task 恢复的完整路由路径（4 种场景）
> - co_submit 的完整实现（保证调用者线程亲和性）
> - AffinityBaton 完整实现（多等待者侵入式链表，1:1 对齐 folly::coro::Baton API，post 走 executor 路由）
> - TimerScheduler 完整设计（HHWheelTimer + add_to_worker 路由）
> - io_uring 集成: I/O 线程是驱动器+分发器，不做数据收发；未来全用户态栈演进方向
> - StatRegistry 集成点全览
> - io_uring 集成路径（IoUringBackend + AsyncIoUringSocket）
> - StatRegistry 集成点全览
> - 七条约束的完整体现映射表
> - 线程模型总览（N+3 线程，无论多少组件都不增长）

## 六、分层架构

```
┌──────────────────────────────────────────────────────┐
│ 业务层                                                │
│  FactorComputer / RiskEngine / AlgoTrader             │
│  co_compute / co_check / co_on_tick                   │
│  统计: StatRegistry::increment / observe_timer        │
├──────────────────────────────────────────────────────┤
│ 编排层 (Orchestration)                                │
│  ExecutionDAG                                         │
│  - 依赖声明、拓扑排序、parallel_levels()               │
│  - 输出: vector<vector<TaskId>>                       │
│  - 对每层: collectAll 提交到调度层                      │
│  - 不知道线程、不知道协程、不知道调度                    │
├──────────────────────────────────────────────────────┤
│ 调度层 (Scheduling)                                   │
│  WorkStealingExecutor (implements folly::Executor)    │
│  - Chase-Lev deque + local stealing                   │
│  - GlobalMPMCQueue                                    │
│  - add_to_worker(id, fn): 线程亲和投递                │
│  - 三级 park: spin → yield → futex (慢降级快恢复)      │
│  - folly::Executor::add() 接口                        │
│  - 协程恢复路由到 last_worker_id                       │
│  - io_event_base_: IoUringBackend (I/O 非阻塞)       │
├──────────────────────────────────────────────────────┤
│ I/O 层                                                │
│  IoUringBackend (EventBase 后端)                      │
│  AsyncIoUringSocket                                   │
│  I/O 线程: 仅驱动 io_uring (SQE/CQE/投递)，不做数据收发│
│  数据收发: 内核 DMA 直写用户态 buffer                  │
│  未来: 可演进为全用户态栈 (DPDK/SPDK)                 │
│  - 磁盘: co_write / co_read / co_fsync                │
│  - 网络: co_accept / co_send / co_recv                │
│  - 零拷贝: ProvidedBufferRing                         │
│  - I/O 完成后通过 add_to_worker 路由回原 worker        │
├──────────────────────────────────────────────────────┤
│ 定时器层                                              │
│  TimerScheduler                                       │
│  - 全局线程 + HHWheelTimer (IoUringBackend)           │
│  - 定时器到期 → add_to_worker(last_worker_id, cont)   │
│  - 协程回到上次执行的 worker 线程                       │
├──────────────────────────────────────────────────────┤
│ 原语层 (Primitives)                                   │
│  folly::coro::Task / Mutex / Semaphore                │
│  folly::coro::collectAll / sleep                      │
│  AffinityBaton: 完整替代 Baton，多等待者，post 走路由  │
├──────────────────────────────────────────────────────┤
│ 统计层                                                │
│  StatRegistry (thread_local LocalHolder + aggregate)  │
│  - increment / set_gauge / observe_timer              │
│  - 所有模块统一使用                                    │
└──────────────────────────────────────────────────────┘
```
