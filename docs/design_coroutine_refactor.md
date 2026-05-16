# C++ 全协程化改造设计方案

> 基于 Folly 协程原语，重构量化系统底层异步机制

## 1. 现状与问题

### 1.1 当前异步机制汇总

| 组件 | 机制 | 问题 |
|------|------|------|
| `ThreadPool::co_submit` | spawn detached thread → `future.wait()` → `handle.resume()` | 每次挂起创建1个线程，协程数=线程数，无意义 |
| `ThreadPool::submit` | `std::promise/future` | 阻塞式获取结果，无法链式组合 |
| `WaveScheduler::execute` | 每波创建 `std::thread` → join | 不复用线程池，创建/销毁开销大 |
| `CronScheduler::tick` | 独立线程 `sleep_for(tick_interval)` | 轮询空转，100%占用1线程 |
| `EventBus::publish_async` | MPSC队列 + 后台线程 + `condition_variable` | 单线程消费，无法与协程衔接 |
| `Logger` 后台刷新 | `cv.wait_for(1ms)` | 1ms空转唤醒 |
| `ConfigManager` 热重载 | `sleep_for(poll_interval)` | 轮询，无法按需唤醒 |
| `TcpConnection` | 同步阻塞 recv/send | 无法与事件循环集成 |

### 1.2 核心矛盾

**`TaskAwaiter::await_suspend`** 的实现是问题之源：

```cpp
// 当前：每次 co_await 创建一个线程来监控 future
void await_suspend(std::coroutine_handle<> handle) noexcept {
    std::thread([this, handle]() mutable {
        future_.wait();     // ← 占用线程阻塞等待
        handle.resume();    // ← 在临时线程上恢复，脱离线程池调度
    }).detach();
}
```

后果：
1. **线程爆炸**：N 个协程挂起 = N 个额外线程
2. **脱离调度**：resume 在 detached 线程上执行，ThreadPool 完全失控
3. **cache 破坏**：协程在不同线程间漂移，L1/L2 cache 全部失效

## 2. 设计目标

| 目标 | 指标 |
|------|------|
| 协程挂起零线程开销 | 挂起 N 个协程不创建任何线程 |
| 恢复在线程池内 | `handle` 入队回 ThreadPool，由 worker 执行 resume |
| DAG 调度全协程化 | 因子计算等依赖链用 `co_await` 组合，不 spawn thread |
| 网络 I/O 异步化 | 接入 io_uring 或 epoll 事件循环 |
| 保留现有 API | 同步 API 保留，内部转发到协程路径 |

## 3. 依赖引入：Folly

### 3.1 选取的 Folly 组件

| 组件 | 作用 | 引入方式 |
|------|------|----------|
| `folly/coro/Task.h` | 协程 Task 类型，lazy 启动 | 头文件 |
| `folly/coro/Baton.h` | 用户态同步原语（替代 condition_variable） | 头文件 |
| `folly/coro/Mutex.h` | 协程互斥锁（挂起不占线程） | 头文件 |
| `folly/coro/Semaphore.h` | 限流（替代线程数限制） | 头文件 |
| `folly/coro/Collect.h` | 并发组合（collectAll, collectAny） | 头文件 |
| `folly/executors/ThreadPoolExecutor.h` | 协程感知线程池 | 头文件 |
| `folly/Unit.h` | void 替代类型 | 头文件 |

### 3.2 CMake 集成

```cmake
# FetchContent 引入 Folly（仅需协程子集）
FetchContent_Declare(
    folly
    GIT_REPOSITORY https://github.com/facebook/folly.git
    GIT_TAG v2025.05.12.00
)
# 仅编译协程+执行器子集，避免全量编译
set(FOLLY_BUILD_SHARED OFF CACHE BOOL "")
set(FOLLY_USE_SIMDHASH OFF CACHE BOOL "")
FetchContent_MakeAvailable(folly)

target_link_libraries(quant_infra PUBLIC Folly::folly)
```

> **轻量替代方案**：如果 Folly 全量编译太重，可以仅提取 `folly/coro/` 子目录（Baton、Mutex、Task、Collect）作为独立 header-only 或小编译单元引入，依赖仅 `boost::context`（fcontext）。

## 4. 核心改造

### 4.1 协程 Task 类型

新建 `cpp/quant/infra/coroutine.h`：

```cpp
#pragma once
#include <folly/coro/Task.h>
#include <folly/coro/Baton.h>
#include <folly/coro/Mutex.h>
#include <folly/coro/Semaphore.h>
#include <folly/coro/Collect.h>

namespace quant::infra {

// 项目级 Task 别名，方便统一替换底层实现
template<typename T = void>
using Task = folly::coro::Task<T>;

using folly::coro::Baton;
using folly::coro::Mutex;
using folly::coro::Semaphore;
using folly::coro::collectAll;
using folly::coro::collectAny;

}  // namespace quant::infra
```

### 4.2 ThreadPool → 协程感知执行器

**改造思路**：ThreadPool 队列同时接受 `Task`（function）和 `coroutine_handle`，worker 取出后按类型分派。

```cpp
// thread_pool.h — 改造后核心差异

class ThreadPool {
public:
    // ── 保留同步接口 ──
    template<CallableTask F>
    auto submit(F&& task) -> std::future<std::invoke_result_t<F>>;

    // ── 协程接口（新）──
    template<typename F>
    folly::coro::Task<std::invoke_result_t<F>> co_submit(F&& task);

    // ── 协程恢复入队（新）──
    void enqueue_handle(std::coroutine_handle<> handle);

private:
    // 队列项：Task 或 coroutine_handle
    struct WorkItem {
        std::variant<std::function<void()>, std::coroutine_handle<>> payload;
    };

    void worker_loop(uint32_t worker_id);
    // ...
};
```

**删除旧 `TaskAwaiter`**——由 `folly::coro::Task` 的 awaiter 替代。

**核心路径**：

```
co_await pool.co_submit(compute_factor)
  → 包装为 folly::coro::Task，挂起时不创建线程
  → 任务入队到 ThreadPool 的 WorkStealingQueue
  → worker 取出执行
  → 执行完成， Baton::post() 唤醒
  → 协程 handle 通过 enqueue_handle() 入队回 ThreadPool
  → 另一个 worker 取出 handle，调用 resume()
```

### 4.3 WaveScheduler 全协程化

**当前问题**：每波 spawn `std::thread`，不复用线程池。

**改造方案**：`execute()` 返回 `Task<WaveExecutionResult>`，内部用 `collectAll` 并发执行同一波次的任务。

```cpp
// wave_scheduler.h — 改造后

class WaveScheduler {
public:
    explicit WaveScheduler(WaveSchedulerConfig config = {});

    // 同步接口（保留，内部启动事件循环）
    WaveExecutionResult execute(TaskGraph& graph);

    // 协程接口（新）
    folly::coro::Task<WaveExecutionResult> co_execute(TaskGraph& graph);

private:
    WaveSchedulerConfig config_;
    ThreadPool* pool_;  // 注入的线程池
};

// 核心实现
folly::coro::Task<WaveExecutionResult> WaveScheduler::co_execute(TaskGraph& graph) {
    auto validation = graph.validate();
    if (!validation.valid) { /* ... */ }

    auto levels = graph.parallel_levels();
    WaveExecutionResult result;
    result.total_tasks = graph.size();

    for (auto& level : levels) {
        // 同一波次内所有任务并发执行
        std::vector<folly::coro::Task<void>> tasks;
        for (auto id : level) {
            tasks.push_back(co_run_task(graph, id));
        }

        // co_await collectAll 等待本波次全部完成
        auto results = co_await folly::coro::collectAllRange(std::move(tasks));
        // 统计结果...
    }

    co_return result;
}
```

**`co_run_task`** 中：
- 任务开始前将 `TaskNode::status` 设为 `kRunning`
- 执行 `task->execute_fn()`（同步逻辑，跑在线程池 worker 上）
- 完成后设为 `kCompleted`

### 4.4 CronScheduler 定时器协程化

**当前问题**：`sleep_for(tick_interval)` 空转。

**改造方案**：用 `folly::coro::sleep` 或 `Baton` 等待精确时间点。

```cpp
// cron_scheduler.h — 改造后核心差异

class CronScheduler {
public:
    // 协程接口
    folly::coro::Task<void> co_tick_loop();

    // 仍保留 start()/stop() 对外接口
    void start();  // 内部启动 co_thread
    void stop();

private:
    folly::coro::Baton stop_baton_;  // 替代 sleep_for + running_ 标志
    // ...
};

// 实现
folly::coro::Task<void> CronScheduler::co_tick_loop() {
    while (!stop_requested_.load()) {
        auto now = /* 当前时间 */;
        int64_t next_tick = find_next_trigger(now);
        if (next_tick > now) {
            // 协程挂起，零线程开销等待精确时间点
            co_await folly::coro::sleep(
                std::chrono::seconds(next_tick - now));
        }
        co_await co_execute_due_jobs();
    }
}
```

### 4.5 EventBus 协程化

```cpp
// event_bus.h 新增协程接口

class EventBus {
public:
    // 同步接口（保留）
    void publish(std::unique_ptr<Event> event);
    void publish_async(std::unique_ptr<Event> event);

    // 协程接口（新）：等待特定事件
    template<typename E>
    folly::coro::Task<E> co_wait(std::string_view channel);

private:
    // 改造：async worker 改为从 MPSC 队列取事件，
    // 然后分发到注册的协程等待者（Baton）
    std::unordered_map<TypeId, std::vector<folly::coro::Baton*>> waiters_;
};
```

### 4.6 网络 I/O 协程化

**长期目标**：替换 `TcpConnection` 的同步 recv/send 为 `co_recv/co_send`，底层接入 io_uring 或 epoll。

**短期方案**：保留同步 API，内部用 `folly::coro::Task` 包装。

```cpp
// tcp_connection.h 新增协程 API

class TcpConnection {
public:
    // 同步接口（保留）
    size_t recv(char* buf, size_t len);

    // 协程接口（新）
    folly::coro::Task<size_t> co_recv(char* buf, size_t len);
    folly::coro::Task<bool> co_send(const char* data, size_t len);

private:
    folly::coro::Baton read_baton_;  // 数据到达时 post
    folly::coro::Baton write_baton_; // 可写时 post
};
```

## 5. 改造分层与优先级

### Phase 1：基础设施（本次改造）

| 文件 | 改动 | 说明 |
|------|------|------|
| `infra/coroutine.h` | **新建** | Task/Baton/Mutex/Semaphore 别名 |
| `infra/thread_pool.h/.cc` | 重构 | 删除 TaskAwaiter，加 `enqueue_handle()`，加 `co_submit()` |
| `infra/event_bus.h/.cc` | 新增 | `co_wait()` 协程等待接口 |
| `CMakeLists.txt` | 修改 | 引入 Folly，链接 `Folly::folly` |

### Phase 2：调度引擎协程化

| 文件 | 改动 | 说明 |
|------|------|------|
| `scheduler/wave_scheduler.h/.cc` | 重构 | `co_execute()` 用 `collectAll` 并行 |
| `scheduler/cron_scheduler.h/.cc` | 重构 | `co_tick_loop()` 用 `co_await sleep` |
| `scheduler/task_graph.h` | 小改 | `execute_fn` 类型改为 `std::function<void()>` → 支持 `co_await` 组合 |

### Phase 3：网络层协程化

| 文件 | 改动 | 说明 |
|------|------|------|
| `network/tcp_connection.h/.cc` | 新增 | `co_recv/co_send` |
| `network/websocket_server.h/.cc` | 新增 | 基于 epoll 事件循环的协程 accept |
| `network/ws_session.h/.cc` | 新增 | 协程化消息收发 |

### Phase 4：上层适配

| 文件 | 改动 | 说明 |
|------|------|------|
| `factor/factor_computer.h/.cc` | 新增 | `co_compute()` 协程版计算 |
| `storage/disk_persistence.h/.cc` | 新增 | `co_flush/co_read` 异步磁盘 |
| `execution/algorithmic_trader.h/.cc` | 新增 | `co_on_tick/co_on_fill` |
| `risk/risk_engine.h/.cc` | 小改 | `co_check()` 异步版 |

## 6. 向后兼容策略

所有改造必须保持同步 API 不变：

```cpp
// 同步 API（保留，现有代码无需改动）
WaveExecutionResult execute(TaskGraph& graph);
size_t recv(char* buf, size_t len);

// 协程 API（新增，新代码使用）
folly::coro::Task<WaveExecutionResult> co_execute(TaskGraph& graph);
folly::coro::Task<size_t> co_recv(char* buf, size_t len);
```

同步 API 内部通过启动临时事件循环来桥接：

```cpp
WaveExecutionResult WaveScheduler::execute(TaskGraph& graph) {
    // 启动事件循环，运行协程，返回结果
    return folly::coro::blockingWait(co_execute(graph));
}
```

## 7. 测试策略

| 层级 | 测试内容 |
|------|----------|
| 单元 | `ThreadPool::co_submit` 正确返回结果，协程挂起/恢复不创建线程 |
| 单元 | `Baton` 等待/唤醒正确 |
| 单元 | `collectAll` 并发执行 DAG 同波次，结果按序收集 |
| 单元 | `co_execute` DAG 依赖顺序正确 |
| 单元 | `CronScheduler::co_tick_loop` 精确触发 |
| 集成 | 因子计算 DAG 端到端协程流水线 |
| 性能 | 协程挂起/恢复延迟 vs 旧 TaskAwaiter 线程方案 |
| 性能 | 1000 协程占线程数对比（应从 1000 → N_workers） |

## 8. 风险与缓解

| 风险 | 缓解 |
|------|------|
| Folly 编译依赖重 | 仅引入 `folly/coro/` 子集，或提取为独立 header |
| C++20 协程编译器支持 | GCC 12+ / Clang 15+ 已稳定支持 |
| 调试困难 | `folly::coro` 提供 `co_await` 堆栈追踪，编译时 `-g -fno-omit-frame-pointer` |
| 并发 bug 引入 | Phase 1 先不加锁数据结构的协程化，仅改 ThreadPool 调度路径 |

## 9. 文件改动清单（Phase 1）

```
新增：
  cpp/quant/infra/coroutine.h          — Task/Baton/Mutex/Semaphore 别名
  cpp/test/coroutine_test.cc            — 协程基础功能测试

修改：
  cpp/quant/infra/thread_pool.h         — 删除 TaskAwaiter，加 enqueue_handle/co_submit
  cpp/quant/infra/thread_pool.cc        — WorkItem variant 队列，worker 识别 handle
  CMakeLists.txt                        — FetchContent Folly，链接 Folly::folly
  cpp/test/thread_pool_test.cc          — 新增 co_submit 测试用例
```