# CLAUDE.md — 开发规范

本文件为 AI 助手和开发者的强制开发规范。所有代码必须遵守以下规则。

## 协程与同步原语规范

### 强制要求：禁止阻塞操作

本系统基于 folly::coro 全协程架构，使用 WorkStealingExecutor 线程亲和调度。**任何阻塞操作都会阻塞整个 worker 线程，破坏调度公平性和延迟。**

**禁止使用**：
- `std::mutex` / `std::lock_guard` / `std::unique_lock`
- `std::shared_mutex` / `std::shared_lock`
- `std::condition_variable` / `std::condition_variable_any`
- `std::this_thread::sleep_for()` / `std::this_thread::yield()`
- 阻塞文件 I/O（read/write，应使用 io_uring 异步路径）
- 阻塞网络 I/O（应使用协程异步 IO）

**必须使用**：
- 互斥锁 → `AffinityMutex`（`co_await mutex.co_lock()` / `co_scoped_lock()`）
- 读写锁 → `AffinitySharedMutex`（`co_shared_lock()` / `co_lock()`）
- 等待/通知 → `AffinityBaton`（`co_await baton.co_wait()`）
- 定时等待 → `co_await sleep(duration)`
- 后台任务 → `AsyncScope::add(co_withExecutor(executor, task()))`
- 并发原语 → `folly::coro::UnboundedQueue` / `BoundedQueue` / `Barrier`

**唯一例外**：外部 SDK 不可侵入的阻塞调用（如 etcd-cpp-apiv3 的 gRPC watch），需在后台线程中执行，通过 `UnboundedQueue` 将事件传递到协程世界。

### 线程亲和原则

所有协程唤醒必须通过 `WorkStealingExecutor::add_to_worker(worker_id, handle)` 路由到原始 worker 线程。禁止直接调用 `handle.resume()`。

### 测试代码

测试代码中允许使用 `blockingWait()` 桥接同步→异步。仅限测试。

## 当前技术债

以下位置仍使用阻塞原语，需优先替换：

| 文件 | 阻塞原语 | 替换为 | 优先级 |
|------|---------|--------|--------|
| `time_series_cache.h` | `std::shared_mutex` | `AffinitySharedMutex` | P0 |
| `segment_index.h` | `std::shared_mutex` | `AffinitySharedMutex` | P0 |
| `disk_persistence.h` | `std::shared_mutex` | `AffinitySharedMutex` | P0 |
| `event_bus.cc` | `std::shared_mutex` | `AffinitySharedMutex` | P1 |
| `config_manager.h` | `std::shared_mutex` | `AffinitySharedMutex` | P1 |

## 代码风格

- 不添加多余注释，只写 WHY 不写 WHAT
- 不创建文档文件除非明确要求
- 不添加未使用的 include
- 修改现有文件优先于创建新文件
- C++20 标准，`#pragma once` 头文件保护
