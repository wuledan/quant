# 技术决策记录 — 协程化改造与 io_uring 集成

> 本文档记录 C++ 量化引擎协程化改造过程中的关键技术决策、方案对比和最终选择理由。

## 1. 协程原语选择

### 1.1 Folly 作为基础依赖

**决策**: 引入 Folly 库，使用 `folly::coro::Task<T>` 作为协程原语

**理由**:
- C++20 `std::coroutine` 仅提供编译器钩子，无调度器、无 executor、无同步原语
- Folly 提供完整的协程生态: `Task<T>`, `Mutex`, `Baton`, `collectAll`, `co_withExecutor`, `blockingWait`
- 与 Facebook 内部量化系统同技术栈，社区验证充分
- folly::coro::Task 是 `TaskWithExecutor`，天然绑定 executor，恢复时不会丢失调度上下文

### 1.2 CoTask 类型别名

```cpp
template<typename T = void>
using CoTask = folly::coro::Task<T>;
```

**理由**: 保持命名空间隔离，未来可切换底层实现而不影响调用方。

---

## 2. Baton 设计: AffinityBaton

### 2.1 问题

`folly::coro::Baton` 的 `post()` 直接恢复等待协程在调用线程上。当调用 `post()` 的线程是 executor worker，而等待协程运行在不同 executor（如 `blockingWait` 的 `ManualExecutor`），导致:
1. 协程恢复在错误的线程上
2. 缓存亲和性丢失
3. 跨 executor 协程恢复导致内存损坏（wide-DAG 场景）

### 2.2 AffinityBaton 设计

**方案**: 自研 `AffinityBaton`，`post(WorkStealingExecutor&)` 通过 executor 路由到等待者的原始 worker 线程

```cpp
class AffinityBaton {
    struct WaiterNode {
        std::coroutine_handle<> handle;
        size_t worker_id;
        WaiterNode* next;
    };
    void post(WorkStealingExecutor& executor);
    void post_direct() noexcept;
};
```

**关键技术点**: 侵入式链表、CAS 无锁入队、ABA-free、post() 先原子摘取整个链表再逐个路由

### 2.3 例外场景: blockingWait + promise/future

**决策**: `WaveScheduler` 和 `ExecutionDAG` 的 level 同步不使用任何 Baton，使用 `std::promise/std::future`

**理由**:
- 协程在 `blockingWait` 的 `ManualExecutor` 上运行，worker_id 为 `kExternalThread`
- AffinityBaton 对 `kExternalThread` 会退化到 inline resume（与 folly::coro::Baton 相同行为）
- `std::future::wait()` 在调用线程阻塞等待，不会跨线程恢复协程
- 消除了 wide-DAG 堆破坏的根本原因

---

## 3. Mutex 策略: CoMutex

### 3.1 folly::coro::Mutex

**决策**: 使用 `folly::coro::Mutex`（别名 `CoMutex`）替代热路径上的 `std::mutex`

**理由**: `std::mutex::lock()` 阻塞线程，在协程上下文中会阻塞整个 executor worker。`co_await mutex.co_scoped_lock()` 挂起协程而非阻塞线程。

### 3.2 co_scoped_lock() vs co_lock()

**关键**: `co_lock()` 返回临时 Awaiter，析构时自动释放锁，在表达式结束后锁立即释放。必须使用 `co_scoped_lock()`:

```cpp
auto lock = co_await mutex_.co_scoped_lock();  // 正确
co_await mutex_.co_lock();                      // 错误，立即释放
```

### 3.3 同步桥接模式

所有协程化组件保留同步 API，内部通过 `blockingWait` 转发:
```cpp
Result<OrderId> create_order(const OrderRequest& req) {
    return blockingWait(co_create_order(req));
}
CoTask<Result<OrderId>> co_create_order(const OrderRequest& req) {
    auto lock = co_await mutex_.co_scoped_lock();
    co_return Result<OrderId>(id);
}
```

---

## 4. io_uring 集成: CoIouring

### 4.1 架构

```
TcpConnection ──▶ CoIouring ◀── Completion Thread
WebSocket            │           (SQE → CQE)
DiskStorage          ▼
              WorkStealingExecutor (线程亲和路由)
```

### 4.2 IoRequest 状态机

| 状态 | 含义 |
|------|------|
| 0 | 提交到 io_uring，等待完成 |
| 1 | 已完成，等待者尚未挂起 |
| 2 | 已完成，等待者已挂起（需要 resume） |

**竞态处理**: 完成先到(0→1)跳过挂起；挂起先到(0→2)由 completion 线程通过 executor 路由 resume。

### 4.3 全局单例

`GlobalExecutor` + `GlobalCoIouring` Meyer's singleton。初始化顺序: `GlobalExecutor::init()` 必须先于 `GlobalCoIouring::init()`。

### 4.4 后备路径

所有 io_uring 协程方法保留同步后备: `ring_ == nullptr` 时回退到同步 I/O。

---

## 5. WorkStealingExecutor

- 每个 worker 独立 MPSC 任务队列
- `add(Task)` 路由到调用者本地队列（thread-affine）
- `add_to_worker(id, Task)` 推送到指定 worker 队列
- 空闲 worker 从其他队列 steal 任务（Chase-Lev deque）

---

## 6. DAG 调度

| 组件 | 职责 |
|------|------|
| TaskGraph | DAG 构建、拓扑排序 |
| WaveScheduler | 逐 wave 并行执行 |
| ExecutionDAG | 高阶编排，支持 coroutine TaskFunc |

**执行模型**: 每 level: promise + 原子计数器 → executor.add() → 最后任务 set_value() → future.wait()

**数据安全**: `std::mutex` 保护 `result` 结构体（vector 非线程安全）。

---

## 7. 关键 Bug 修复

### 7.1 std::mutex 跨 co_await (WebSocketServer)

**问题**: 持有 `std::mutex` 时 `co_await`，协程恢复在另一线程导致 unlock 在错误线程（UB）。

**修复**: 锁内提取 fd，锁外执行 co_await:
```cpp
int fd = -1;
{ std::lock_guard lk(mutex); fd = it->second; }
co_await ring_->co_send(fd, ...);
```

### 7.2 folly::coro::Baton 堆破坏 (Wide-DAG)

见 §2.3。修复: promise/future 替代 Baton。

---

## 8. 遗留问题

1. **TimerScheduler**: 接入 CoIouring 的 timeout SQE 可优化
2. **io_uring open/stat**: 未实现，文件打开在同步路径
3. **WebSocket accept**: 未协程化，使用阻塞 accept
4. **批量提交**: io_uring SQE 可批量提交减少 syscall

---

## 9. 未来工作

- WaveScheduler/ExecutionDAG 纯协程模式，彻底移除 blockingWait
- TimerScheduler 整合到 CoIouring timeout SQE
- WebSocket accept 协程化
- io_uring IOSQE_IO_LINK 链式提交
- 前端页面与后端 API 集成
