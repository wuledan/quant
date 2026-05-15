# 量化投资系统 — C++ 基础组件架构设计

> 版本：v1.0 | 日期：2026-05-15 | 模块：C++ 基础组件 / 公共库 / 调度引擎 / 互操作层

---

## 目录

1. [公共基础库](#1-公共基础库)
   - 1.1 [线程池](#11-线程池work-stealing)
   - 1.2 [内存池](#12-内存池自定义分配器)
   - 1.3 [对象池](#13-对象池预分配复用)
   - 1.4 [时间工具](#14-时间工具交易日历时间戳管理)
   - 1.5 [错误码体系](#15-错误码体系统一错误码错误链)
2. [配置管理](#2-配置管理)
3. [结构化日志系统](#3-结构化日志系统)
4. [网络层](#4-网络层)
5. [C++/Python 互操作层](#5-cpython-互操作层)
6. [调度引擎](#6-调度引擎)
7. [目录结构与文件组织](#7-目录结构与文件组织)

---

## 1. 公共基础库

### 1.1 线程池（Work-Stealing）

#### 核心设计

基于 C++20 的 work-stealing 线程池，每个工作线程拥有本地双端队列（deque），空闲线程可从其他线程队列尾部窃取任务，减少全局锁争用。

#### 关键接口

```cpp
// thread_pool.h
#pragma once

#include <concepts>
#include <coroutine>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>

namespace quant::infra {

// ── 任务概念 ──
template<typename F>
concept CallableTask = std::is_invocable_v<std::decay_t<F>>;

// ── 任务类型擦除 ──
class Task {
public:
    template<CallableTask F>
    explicit Task(F&& fn);
    void execute() noexcept { func_(); }
private:
    std::function<void()> func_;
};

// ── 调度策略 ──
enum class SchedulePolicy {
    RoundRobin,     // 轮询分配
    LeastLoaded,    // 最少负载优先
    WorkStealing,   // 本地队列 + 窃取（默认）
};

// ── 线程池配置 ──
struct ThreadPoolConfig {
    uint32_t worker_count = 0;          // 0 = 自动检测 std::thread::hardware_concurrency()
    uint32_t local_queue_capacity = 256;
    uint32_t steal_attempts = 4;        // 窃取失败重试次数
    SchedulePolicy policy = SchedulePolicy::WorkStealing;
    std::string thread_name_prefix = "quant-pool";
};

// ── Awaiter: co_await 提交任务 ──
template<typename T>
class TaskAwaiter {
public:
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) noexcept;
    T await_resume();
private:
    std::future<T> future_;
    ThreadPool* pool_;
};

// ── 线程池主类 ──
class ThreadPool {
public:
    explicit ThreadPool(const ThreadPoolConfig& cfg = {});
    ~ThreadPool();

    // 禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // ── 同步提交 ──
    template<CallableTask F>
    auto submit(F&& task) -> std::future<std::invoke_result_t<F>>;

    // ── 协程提交 ──
    template<CallableTask F>
    auto co_submit(F&& task) -> TaskAwaiter<std::invoke_result_t<F>>;

    // ── 批量提交 ──
    template<std::ranges::range R>
    auto submit_batch(R&& tasks) -> std::vector<std::future<void>>;

    // ── 生命周期 ──
    void start();
    void stop() noexcept;        // 等待所有任务完成
    void force_stop() noexcept;  // 放弃未执行任务

    // ── 运行时统计 ──
    struct Stats {
        uint64_t tasks_submitted;
        uint64_t tasks_completed;
        uint64_t tasks_stolen;
        uint64_t queue_overflow_count;
    };
    Stats stats() const noexcept;

    uint32_t worker_count() const noexcept;
    bool is_running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── 全局单例（进程级默认线程池） ──
ThreadPool& default_thread_pool();

}  // namespace quant::infra
```

#### 数据流描述

```
Submitter                    ThreadPool                    Workers
   │                             │                            │
   ├─ submit(task) ──────────────►│                            │
   │                             ├── push to local queue ──► Worker[least_loaded]
   │                             │                            ├── try_pop_front() → execute
   │                             │                            │
   │                             │            Steal Path      │
   │                             │◄═══════════════════════════ Worker[idle]
   │                             │   steal from random peer    ├── try_pop_back() → execute
   │                             │                            │
   ├─ co_submit(task) ───────► Resumable coroutine           │
   │                             ├── post to pool              │
   │                             │   ... on complete ────────► resume coroutine_handle
   │                             │                            │
   ├─ submit_batch(tasks) ──────►├── partition across workers │
   │                             │                            ├── each worker drains its partition
```

#### 线程模型与并发策略

| 要素 | 策略 |
|------|------|
| 线程数 | 默认 = `hardware_concurrency`，可配置 |
| 本地队列 | 每 worker 一个 `boost::lockfree::deque`（无锁），容量 256 |
| 全局队列 | 超出本地容量的溢出任务进入全局 `moodycamel::ConcurrentQueue` |
| 窃取策略 | 随机选 target → 尝试 steal 尾部 → 失败则重试最多 4 次 |
| 停止语义 | `stop()` 等待排空；`force_stop()` 立即终止 |
| 协程桥接 | `co_submit` 返回 `TaskAwaiter<T>`，任务完成后 resume 调用方协程 |
| 统计 | 原子计数器记录 submitted / completed / stolen / overflow |

#### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| 任务提交延迟（无争用） | < 500 ns |
| 任务提交延迟（高争用 16 线程） | < 2 μs |
| Work-stealing 窃取延迟 | < 1 μs |
| 吞吐量（简单任务） | > 10 M tasks/s |
| 全局队列争用时的退避开销 | < 5% cpu 时间 |

---

### 1.2 内存池（自定义分配器）

#### 核心设计

分层内存池：小对象（≤256B）走 thread-local 最小单元缓存，中对象走 centralized free-list，大对象直接转发 `malloc`/`mmap`。实现 `std::pmr::memory_resource` 接口以兼容 STL 容器。

#### 关键接口

```cpp
// memory_pool.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>

namespace quant::infra {

// ── 小对象缓存配置 ──
struct SmallObjectConfig {
    uint32_t max_object_size = 256;       // 超过此大小走 central pool
    uint32_t slot_count = 8;             // size class 数量（8/16/32/64/128/256）
    uint32_t per_thread_cache_lines = 64; // 每 thread-local 缓存行数
};

// ── 内存池统计 ──
struct MemoryPoolStats {
    size_t total_allocated;
    size_t total_freed;
    size_t current_in_use;
    size_t peak_usage;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t cache_hit_count;
    uint64_t cache_miss_count;
    double cache_hit_rate() const noexcept;
};

// ── 主内存池 ──
class QuantMemoryResource : public std::pmr::memory_resource {
public:
    explicit QuantMemoryResource(const SmallObjectConfig& cfg = {});
    ~QuantMemoryResource() override;

    // ── 预热：提前分配指定量内存 ──
    void warmup(size_t total_bytes);

    // ── 统计 ──
    MemoryPoolStats stats() const noexcept;

    // ── 重置：归还所有缓存但不释放底层块 ──
    void reset() noexcept;

protected:
    void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void* ptr, size_t bytes, size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── 便捷分配器 ──
template<typename T>
class QuantAllocator {
public:
    using value_type = T;

    explicit QuantAllocator(QuantMemoryResource* mr) noexcept : mr_(mr) {}

    template<typename U>
    QuantAllocator(const QuantAllocator<U>& other) noexcept : mr_(other.resource()) {}

    T* allocate(size_t n);
    void deallocate(T* ptr, size_t n) noexcept;

    QuantMemoryResource* resource() const noexcept { return mr_; }

    bool operator==(const QuantAllocator& other) const noexcept {
        return mr_ == other.mr_;
    }

private:
    QuantMemoryResource* mr_;
};

// ── 全局单例 ──
QuantMemoryResource& global_memory_resource();

// ──便捷构造函数 ──
template<typename T>
using PmrVector = std::pmr::vector<T>;

template<typename T, typename... Args>
std::unique_ptr<T> make_quant(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

}  // namespace quant::infra
```

#### 数据流描述

```
allocate(bytes, alignment):
  │
  ├─ bytes <= 256 ?
  │   ├── YES → thread-local size class cache
  │   │         ├── cache hit → return ptr (< 50ns)
  │   │         └── cache miss → refill from central free-list → return ptr
  │   └── NO
  │       ├── bytes <= 4KB → central free-list（分 span 管理）
  │       └── bytes > 4KB  → direct mmap / malloc
  │
deallocate(ptr, bytes, alignment):
  │
  ├─ bytes <= 256 → return to thread-local cache
  │                  └── cache full → flush batch to central free-list
  ├─ bytes <= 4KB → return to central free-list
  └── bytes > 4KB  → direct munmap / free
```

#### 线程模型与并发策略

| 要素 | 策略 |
|------|------|
| 小对象 | thread-local 无锁缓存，无任何跨线程争用 |
| 中对象 | centralized lock-free free-list (based on `boost::lockfree::stack`) |
| 大对象 | 直接调用系统分配器 |
| 对齐 | 支持 8/16/32/64 字节对齐，大块分配页对齐 (4KB) |
| 内存块 | 从 OS 申请大块（以 1MB 为单位），内部分割管理 |

#### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| 小对象分配延迟 | < 50 ns（thread-local hit） |
| 中对象分配延迟 | < 200 ns |
| 大对象分配延迟 | ≈ malloc |
| 缓存命中率 | > 90%（稳态运行场景） |
| 内存碎片率 | < 5% |
| 内存开销（元数据） | < 分配量的 2% |

---

### 1.3 对象池（预分配+复用）

#### 核心设计

为高频创建/销毁的对象（如行情快照、委托对象、信号事件）提供预分配 + 复用池。归还对象时调用 reset() 而非析构，下次获取时跳过构造。

#### 关键接口

```cpp
// object_pool.h
#pragma once

#include <cassert>
#include <concepts>
#include <memory>
#include <mutex>
#include <vector>

namespace quant::infra {

// ── 可重置概念 ──
template<typename T>
concept Resettable = requires(T& obj) {
    { obj.reset() } -> std::same_as<void>;
};

// ── 对象池配置 ──
template<typename T>
struct ObjectPoolConfig {
    size_t initial_capacity = 1024;   // 预分配数量
    size_t grow_factor = 2;            // 扩容倍数
    size_t max_capacity = 0;           // 0 = 无上限
    bool thread_safe = true;           // 是否线程安全
};

// ── 对象池 ──
template<Resettable T>
class ObjectPool {
public:
    using Config = ObjectPoolConfig<T>;

    explicit ObjectPool(const Config& cfg = {});
    ~ObjectPool();

    // ── 获取对象 ──
    std::shared_ptr<T> acquire();

    // ── 归还对象（shared_ptr 析构时自动归还） ──
    // 通过自定义 deleter 实现，用户无需显式调用

    // ── 预热 ──
    void warmup(size_t count);

    // ── 统计 ──
    struct Stats {
        size_t total_allocated;
        size_t total_in_use;
        size_t total_available;
        size_t peak_in_use;
        uint64_t acquire_count;
        uint64_t release_count;
    };
    Stats stats() const noexcept;

    // ── 缩减：归还多余预分配 ──
    void shrink_to_fit();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── 典型用法示例 ──
// 行情快照对象池
struct MarketSnapshot {
    std::string symbol;
    double bid_price;
    double ask_price;
    int64_t bid_volume;
    int64_t ask_volume;
    int64_t timestamp_ns;

    void reset() {
        symbol.clear();
        bid_price = ask_price = 0.0;
        bid_volume = ask_volume = 0;
        timestamp_ns = 0;
    }
};

using SnapshotPool = ObjectPool<MarketSnapshot>;

}  // namespace quant::infra
```

#### 数据流描述

```
acquire():
  │
  ├─ free_list 非空?
  │   ├── YES → pop from free_list → 构造 shared_ptr(custom_deleter) → return
  │   └── NO  → 检查是否达到 max_capacity
  │              ├── 未达上限 → allocate new object block → push N objects → acquire
  │              └── 达到上限 → 阻塞等待 或 返回空（可配置策略）

release (via shared_ptr deleter):
  │
  ├─ obj.reset()  // 重置对象状态
  ├─ push obj back to free_list
  └─ 如果有线程在等待 acquire → 通知
```

#### 线程模型与并发策略

| 要素 | 策略 |
|------|------|
| 线程安全 | 默认使用 `std::mutex` 保护 free-list；性能敏感场景可用 `spinlock` |
| 大批量获取 | `acquire_batch(n)` 一次取出多个，减少锁次数 |
| 对象生命周期 | `shared_ptr` + custom deleter 自动回收 |
| 扩容 | 按 `grow_factor` 倍增，从内存池分配 |

#### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| acquire 延迟 | < 100 ns（free-list hit） |
| release 延迟 | < 100 ns |
| 扩容时延迟 | < 10 μs（分配 1K 对象块） |
| 内存开销 | 对象 + 1 个指针/对象（free-list link） |

---

### 1.4 时间工具（交易日历、时间戳管理）

#### 核心设计

统一时间管理，支持交易日历查询、纳秒精度时间戳、时区处理、A股交易时间段判定。

#### 关键接口

```cpp
// time_utils.h
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace quant::infra {

// ── 纳秒精度时间戳 ──
class Timestamp {
public:
    constexpr Timestamp() noexcept : ns_(0) {}
    explicit constexpr Timestamp(int64_t nanoseconds) noexcept : ns_(nanoseconds) {}

    // 从 time_point 构造
    static Timestamp now() noexcept;
    static Timestamp from_unix_seconds(double sec) noexcept;
    static Timestamp from_unix_millis(int64_t ms) noexcept;
    static Timestamp from_iso8601(std::string_view str);

    // ── 转换 ──
    constexpr int64_t unix_nanos() const noexcept { return ns_; }
    constexpr int64_t unix_micros() const noexcept { return ns_ / 1'000; }
    constexpr int64_t unix_millis() const noexcept { return ns_ / 1'000'000; }
    constexpr int64_t unix_seconds() const noexcept { return ns_ / 1'000'000'000; }
    double to_double() const noexcept;

    std::string to_iso8601() const;           // "2026-05-15T09:30:00.123456789+08:00"
    std::string to_date_str() const;           // "2026-05-15"
    std::string to_time_str() const;           // "09:30:00.123456789"

    // ── 运算 ──
    constexpr Timestamp operator+(const std::chrono::nanoseconds& dur) const noexcept;
    constexpr Timestamp operator-(const std::chrono::nanoseconds& dur) const noexcept;
    constexpr std::chrono::nanoseconds operator-(Timestamp other) const noexcept;
    constexpr auto operator<=>(const Timestamp&) const noexcept = default;

private:
    int64_t ns_;  // Unix epoch 纳秒
};

// ── 时间区间 ──
struct TimeInterval {
    Timestamp start;
    Timestamp end;
};

// ── 交易时段 ──
enum class MarketPhase {
    PreMarket,      // 盘前集合竞价 (09:15-09:25)
    OpenAuction,     // 开盘集合竞价 (09:25-09:30)
    MorningSession, // 早盘连续竞价 (09:30-11:30)
    LunchBreak,     // 午间休市 (11:30-13:00)
    AfternoonSession,// 午盘连续竞价 (13:00-14:57)
    ClosingAuction, // 收盘集合竞价 (14:57-15:00)
    AfterHours,     // 盘后 (15:00+)
    Closed,         // 非交易日
};

// ── 交易日历 ──
class TradingCalendar {
public:
    // ── 加载日历数据 ──
    static std::shared_ptr<TradingCalendar> load(
        std::string_view calendar_name = "SSE"  // 上交所
    );

    // ── 交易日判断 ──
    bool is_trading_day(const Timestamp& ts) const noexcept;
    bool is_trading_day(int year, int month, int day) const noexcept;

    // ── 交易时段 ──
    MarketPhase phase_at(const Timestamp& ts) const noexcept;

    // ── 下/上一个交易日 ──
    Timestamp next_trading_day(const Timestamp& ts) const noexcept;
    Timestamp prev_trading_day(const Timestamp& ts) const noexcept;

    // ── 交易日内的时间区间 ──
    TimeInterval morning_session(int year, int month, int day) const noexcept;
    TimeInterval afternoon_session(int year, int month, int day) const noexcept;

    // ── 统计 ──
    size_t trading_day_count(Timestamp from, Timestamp to) const noexcept;
    std::vector<Timestamp> trading_days(Timestamp from, Timestamp to) const;

    // ── 日历名称 ──
    std::string_view name() const noexcept;

private:
    TradingCalendar() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── 时钟接口（可注入用于测试） ──
class Clock {
public:
    virtual ~Clock() = default;
    virtual Timestamp now() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
};

class SystemClock : public Clock {
public:
    Timestamp now() const noexcept override { return Timestamp::now(); }
    std::string_view name() const noexcept override { return "system"; }
};

class SimulatedClock : public Clock {
public:
    explicit SimulatedClock(Timestamp start_time) : current_(start_time) {}
    Timestamp now() const noexcept override { return current_; }
    std::string_view name() const noexcept override { return "simulated"; }
    void advance(std::chrono::nanoseconds dur) noexcept;
    void set(Timestamp ts) noexcept;
private:
    Timestamp current_;
};

// ── 全局时钟 ──
void set_global_clock(std::shared_ptr<Clock> clock);
Clock& global_clock();

}  // namespace quant::infra
```

#### 数据流描述

```
行情/策略模块
    │
    ├─ Timestamp::now() / global_clock().now()
    │     ├── SystemClock → clock_gettime(CLOCK_MONOTONIC) + offset
    │     └── SimulatedClock → 返回模拟时间（回测用）
    │
    ├─ calendar.is_trading_day(ts)
    │     └── 查日历数据（内存 hash_set<YYYYMMDD>）
    │
    ├─ calendar.phase_at(ts)
    │     └── 判定当前交易时段
    │         ├── PreMarket → 等待开盘
    │         ├── MorningSession → 正常交易
    │         └── Closed → 跳过
    │
    └─ calendar.next_trading_day(ts)
          └── 二分查找日历表
```

#### 线程模型与并发策略

| 要素 | 策略 |
|------|------|
| 日历数据 | `shared_ptr<Impl>`，加载后只读，多线程安全 |
| 时钟 | `SimulatedClock` 非线程安全，仅回测单线程使用；`SystemClock` 无状态，线程安全 |
| 全局时钟 | `std::atomic<shared_ptr>` 或 RCU 语义切换（极少更新） |

#### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| `Timestamp::now()` | < 30 ns |
| `is_trading_day()` | < 10 ns（hash lookup） |
| `phase_at()` | < 20 ns |
| 日历内存 | < 500 KB（20 年交易日数据） |

---

### 1.5 错误码体系（统一错误码定义、错误链）

#### 核心设计

统一错误码定义，按模块分段编码。支持错误链（cause chain），方便追踪根因。遵循 `std::error_code` / `std::error_category` 标准。

#### 关键接口

```cpp
// error_codes.h
#pragma once

#include <cstdint>
#include <error_code>
#include <string>
#include <string_view>
#include <source_location>
#include <exception>
#include <vector>
#include <memory>

namespace quant::infra {

// ── 错误码分段 ──
// 0x00XX0000 — 基础层
// 0x01XX0000 — 数据层
// 0x02XX0000 — 策略层
// 0x03XX0000 — 交易层
// 0x04XX0000 — 风控层
// 0x05XX0000 — 网络层
// 0x06XX0000 — 调度层

enum class ErrorCode : uint32_t {
    // ── 通用 (0x0001xxxx) ──
    OK                      = 0,
    Unknown                 = 0x00010001,
    InvalidArgument         = 0x00010002,
    Timeout                 = 0x00010003,
   OutOfResource           = 0x00010004,
    NotInitialized          = 0x00010005,
    AlreadyInitialized      = 0x00010006,
    Cancelled               = 0x00010007,

    // ── 基础库 (0x0002xxxx) ──
    ThreadPoolStopped       = 0x00020001,
    ThreadPoolQueueFull     = 0x00020002,
    MemoryPoolExhausted     = 0x00020003,
    ObjectPoolExhausted     = 0x00020004,

    // ── 数据层 (0x0001xxxx) ──
    DataNotFound             = 0x01000001,
    DataFormatError          = 0x01000002,
    DataValidationError      = 0x01000003,
    DataSourceUnavailable    = 0x01000004,

    // ── 策略层 (0x0002xxxx) ──
    StrategyNotFound         = 0x02000001,
    StrategyParamError       = 0x02000002,
    StrategyRuntimeError     = 0x02000003,

    // ── 交易层 (0x0003xxxx) ──
    OrderRejected            = 0x03000001,
    OrderTimeout             = 0x03000002,
    BrokerConnectionLost     = 0x03000003,
    InsufficientFunds        = 0x03000004,

    // ── 风控层 (0x0004xxxx) ──
    RiskLimitExceeded        = 0x04000001,
    RiskRuleViolation        = 0x04000002,

    // ── 网络层 (0x0005xxxx) ──
    ConnectionRefused        = 0x05000001,
    ConnectionTimeout        = 0x05000002,
    WSHandshakeFailed        = 0x05000003,

    // ── 调度层 (0x0006xxxx) ──
    TaskDependencyCycle      = 0x06000001,
    TaskExecutionFailed      = 0x06000002,
    TaskRetryExhausted       = 0x06000003,
};

// ── error_category ──
class QuantErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "quant"; }
    std::string message(int code) const override;
};

const QuantErrorCategory& quant_error_category();

inline std::error_code make_error_code(ErrorCode e) {
    return {static_cast<int>(e), quant_error_category()};
}

// ── 错误链节点 ──
class ErrorNode {
public:
    ErrorNode(ErrorCode code,
              std::string message,
              std::source_location loc = std::source_location::current(),
              std::unique_ptr<ErrorNode> cause = nullptr)
        : code_(code)
        , message_(std::move(message))
        , location_(loc)
        , cause_(std::move(cause))
    {}

    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    const std::source_location& location() const noexcept { return location_; }
    const ErrorNode* cause() const noexcept { return cause_.get(); }

    // ── 格式化输出 ──
    std::string to_string() const;

private:
    ErrorCode code_;
    std::string message_;
    std::source_location location_;
    std::unique_ptr<ErrorNode> cause_;
};

// ── 构建错误链的便捷函数 ──
template<typename... Args>
std::unique_ptr<ErrorNode> make_error(ErrorCode code, Args&&... args) {
    return std::make_unique<ErrorNode>(code, std::forward<Args>(args)...);
}

std::unique_ptr<ErrorNode> chain_error(
    ErrorCode code,
    std::string message,
    std::unique_ptr<ErrorNode> cause,
    std::source_location loc = std::source_location::current()
);

// ── Result 类型 ──
template<typename T>
class Result {
public:
    // 成功构造
    Result(T value) : value_(std::move(value)), error_(nullptr) {}  // NOLINT
    // 失败构造
    Result(std::unique_ptr<ErrorNode> error) : error_(std::move(error)) {}  // NOLINT
    Result(ErrorCode code, std::string msg)
        : error_(std::make_unique<ErrorNode>(code, std::move(msg))) {}

    // ── 访问 ──
    bool ok() const noexcept { return error_ == nullptr; }
    explicit operator bool() const noexcept { return ok(); }

    const T& value() const& { return value_.value(); }
    T& value() & { return value_.value(); }
    T&& value() && { return std::move(value_.value()); }

    const ErrorNode* error() const noexcept { return error_.get(); }

    // ── 链式操作 ──
    template<typename Func>
    auto map(Func&& f) -> Result<std::invoke_result_t<Func, T>>;

    template<typename Func>
    auto and_then(Func&& f) -> std::invoke_result_t<Func, T>;

private:
    // 使用 std::optional 避免默认构造要求
    std::optional<T> value_;
    std::unique_ptr<ErrorNode> error_;
};

// Result<void> 特化
template<>
class Result<void> {
public:
    Result() : error_(nullptr) {}
    Result(std::unique_ptr<ErrorNode> error) : error_(std::move(error)) {}
    Result(ErrorCode code, std::string msg)
        : error_(std::make_unique<ErrorNode>(code, std::move(msg))) {}

    bool ok() const noexcept { return error_ == nullptr; }
    explicit operator bool() const noexcept { return ok(); }
    const ErrorNode* error() const noexcept { return error_.get(); }

private:
    std::unique_ptr<ErrorNode> error_;
};

// ── 异常桥接（跨 C++/Python 边界时使用） ──
class QuantException : public std::exception {
public:
    QuantException(std::unique_ptr<ErrorNode> error);
    const char* what() const noexcept override;
    const ErrorNode& error_node() const noexcept;
private:
    std::unique_ptr<ErrorNode> error_;
    mutable std::string what_message_;
};

}  // namespace quant::infra

// 使 ErrorCode 可用于 std::error_code
namespace std {
template<>
struct is_error_code_enum<quant::infra::ErrorCode> : true_type {};
}  // namespace std
```

#### 数据流描述

```
底层错误                               高层错误
  │                                      │
  ├─ DataValidationError("字段缺失")      │
  │    └── cause: DataSourceUnavailable   │
  │          └── cause: ConnectionTimeout │
  │                                      │
  └──→ chain_error(                      │
        StrategyRuntimeError,            │
        "策略执行失败",                   │
        chain_error(                      │
          DataNotFound,                  │
          "数据查询失败",                 │
          make_error(DataSourceUnavailable, "数据源不可达")
        )
      )

格式化输出:
  [0x02000003] StrategyRuntimeError: 策略执行失败
    at strategy_runner.cpp:142
  Caused by: [0x01000001] DataNotFound: 数据查询失败
    at data_query.cpp:88
  Caused by: [0x01000004] DataSourceUnavailable: 数据源不可达
    at data_source.cpp:201
```

#### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| `Result<T>` 构造（成功路径） | ≈ `std::optional` 开销 |
| `Result<T>` 构造（失败路径） | < 500 ns（含字符串分配） |
| 错误链格式化 | 仅在最终输出时执行，不影响热路径 |
| `ErrorCode` 比较 | 编译期常量，零开销 |

---

## 2. 配置管理

### 核心设计

分层配置：全局 → 模块 → 策略级，后者覆盖前者。支持 YAML/TOML 格式，运行时热更新，配置变更通知回调和验证机制。

### 关键接口

```cpp
// config_manager.h
#pragma once

#include <any>
#include <chrono>
#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace quant::infra {

// ── 配置值类型 ──
using ConfigValue = std::variant<
    bool, int64_t, double, std::string,
    std::vector<bool>, std::vector<int64_t>,
    std::vector<double>, std::vector<std::string>
>;

// ── 配置层级 ──
enum class ConfigLevel {
    Global,     // 全局配置（如日志级别、线程池大小）
    Module,     // 模块级配置（如数据源地址）
    Strategy,   // 策略级配置（如均线周期、阈值）
};

// ── 配置验证器 ──
template<typename T>
class ConfigValidator {
public:
    ConfigValidator& range(T min_val, T max_val) { min_ = min_val; max_ = max_val; return *this; }
    ConfigValidator& default_value(T val) { default_ = std::move(val); return *this; }
    ConfigValidator& description(std::string desc) { desc_ = std::move(desc); return *this; }

    std::optional<T> default_value() const { return default_; }
    bool validate(const T& value) const;
    std::string description() const { return desc_; }

private:
    std::optional<T> default_;
    std::optional<T> min_;
    std::optional<T> max_;
    std::string desc_;
};

// ── 配置变更事件 ──
struct ConfigChangeEvent {
    std::string key;           // "module.submodule.param"
    ConfigValue old_value;
    ConfigValue new_value;
    ConfigLevel level;
    std::string source;        // "file" / "api" / "runtime"
};

// ── 配置变更回调 ──
using ConfigCallback = std::function<void(const ConfigChangeEvent&)>;

// ── 配置来源 ──
class ConfigSource {
public:
    virtual ~ConfigSource() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual Result<std::map<std::string, ConfigValue>> load() = 0;
    virtual Result<void> save(const std::map<std::string, ConfigValue>& kv) = 0;
};

// ── YAML 配置源 ──
class YamlConfigSource : public ConfigSource {
public:
    explicit YamlConfigSource(std::string_view file_path);
    std::string_view name() const noexcept override { return "yaml"; }
    Result<std::map<std::string, ConfigValue>> load() override;
    Result<void> save(const std::map<std::string, ConfigValue>& kv) override;
};

// ── TOML 配置源 ──
class TomlConfigSource : public ConfigSource {
public:
    explicit TomlConfigSource(std::string_view file_path);
    std::string_view name() const noexcept override { return "toml"; }
    Result<std::map<std::string, ConfigValue>> load() override;
    Result<void> save(const std::map<std::string, ConfigValue>& kv) override;
};

// ── 配置管理器 ──
class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    // ── 加载配置 ──
    Result<void> load_global(std::unique_ptr<ConfigSource> source);
    Result<void> load_module(std::string_view module,
                              std::unique_ptr<ConfigSource> source);
    Result<void> load_strategy(std::string_view strategy_id,
                               std::unique_ptr<ConfigSource> source);

    // ── 读取配置（分层覆盖：Strategy > Module > Global） ──
    template<typename T>
    Result<T> get(std::string_view key) const;

    // ── 带 fallback 的读取 ──
    template<typename T>
    T get_or(std::string_view key, T&& default_val) const;

    // ── 注册验证规则 ──
    template<typename T>
    void register_validator(std::string_view key, ConfigValidator<T> validator);

    // ── 运行时修改（触发验证 + 通知） ──
    Result<void> set(std::string_view key, ConfigValue value, ConfigLevel level);

    // ── 订阅配置变更 ──
    uint64_t subscribe(std::string_view key_pattern, ConfigCallback callback);
    void unsubscribe(uint64_t subscription_id);

    // ── 热更新：文件变化时重新从源加载 ──
    void enable_hot_reload(std::chrono::seconds poll_interval = 5);
    void disable_hot_reload();

    // ── 导出当前全量配置（调试用） ──
    std::map<std::string, ConfigValue> export_all() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── 全局单例 ──
ConfigManager& global_config_manager();

}  // namespace quant::infra
```

### 数据流描述

```
                     ┌─────────────────────┐
                     │   Config Files       │
                     │  (YAML / TOML)       │
                     └────────┬────────────┘
                              │ load()
                              ▼
┌──────────────────────────────────────────────────────────────┐
│                     ConfigManager                             │
│                                                               │
│   ┌───────────┐  ┌───────────┐  ┌────────────────────────┐  │
│   │  Global   │  │  Module   │  │      Strategy           │  │
│   │  Config   │  │  Config   │  │  (per strategy_id)      │  │
│   │  Layer    │  │  Layer    │  │  Layer (最高优先级)      │  │
│   └─────┬─────┘  └─────┬─────┘  └──────────┬─────────────┘  │
│         │              │                   │                  │
│         └──────────────┼───────────────────┘                  │
│                        │                                      │
│                        ▼                                      │
│              ┌─────────────────┐    ┌──────────────────┐      │
│              │  Merged Config  │◄───│ Validator Registry│      │
│              │  (effective)    │    │ (range, default)  │      │
│              └────────┬────────┘    └──────────────────┘      │
│                       │                                       │
│                       ▼                                       │
│              ┌─────────────────┐                              │
│              │  Change Notify  │ ──── callbacks ──────► 消费者 │
│              │  (key_pattern)  │                              │
│              └─────────────────┘                              │
└───────────────────────────────────────────────────────────────┘
         ▲
         │ set() (runtime update)
         │
   HTTP API / Strategy Runtime
```

### 线程模型与并发策略

| 要素 | 策略 |
|------|------|
| 读取 | 无锁（`shared_mutex` → `shared_lock`），分层查找顺序：Strategy → Module → Global |
| 写入 | `unique_lock` 保护，变更后发布通知 |
| 热更新 | 独立线程定期轮询文件 mtime；`inotify` 可作为优化选项 |
| 回调 | 写操作同线程顺序调用回调；异步需用户自行调度 |
| 验证 | 写入前验证，不通过则 reject 并返回错误 |

### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| `get<T>(key)` 读取延迟 | < 100 ns（命中缓存） |
| `set()` 写入延迟（含验证+通知） | < 10 μs |
| 热更新检测延迟 | < 5s（轮询间隔可配） |
| 内存占用 | < 10 MB（1000 个配置项） |

---

## 3. 结构化日志系统

### 核心设计

异步日志写入，环形缓冲 + 后台刷盘线程。结构化 JSON 输出兼容 ELK 采集。集成轻量 Metrics（Counter / Histogram / Gauge）。

### 关键接口

```cpp
// logger.h
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>

namespace quant::infra {

// ── 日志级别 ──
enum class LogLevel : uint8_t {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    FATAL = 5,
};

constexpr std::string_view level_name(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
    }
    return "UNKNOWN";
}

// ── 结构化字段 ──
struct LogField {
    std::string key;
    std::string value;  // JSON 编码后的字符串
};

// ── 日志记录 ──
struct LogRecord {
    Timestamp timestamp;
    LogLevel level;
    std::source_location location;
    std::string message;
    std::vector<LogField> fields;
    std::string thread_id;  // 缓存的线程标识
};

// ── 日志输出器 ──
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() = 0;
};

// ── JSON 输出到文件 ──
class JsonFileSink : public LogSink {
public:
    explicit JsonFileSink(std::string_view file_path, size_t rotate_size_mb = 100);
    void write(const LogRecord& record) override;
    void flush() override;
};

// ── 控制台彩色输出 ──
class ConsoleSink : public LogSink {
public:
    void write(const LogRecord& record) override;
    void flush() override;
};

// ── 日志器 ──
class Logger {
public:
    explicit Logger(std::string_view name);
    ~Logger();

    // ── 日志方法 ──
    template<typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args,
               std::source_location loc = std::source_location::current());

    template<typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args,
               std::source_location loc = std::source_location::current());

    template<typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args,
              std::source_location loc = std::source_location::current());

    template<typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args,
              std::source_location loc = std::source_location::current());

    template<typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args,
               std::source_location loc = std::source_location::current());

    template<typename... Args>
    void fatal(std::format_string<Args...> fmt, Args&&... args,
               std::source_location loc = std::source_location::current());

    // ── 结构化日志（链式 builder） ──
    class LogBuilder {
    public:
        LogBuilder& field(std::string_view key, std::string_view value);
        LogBuilder& field(std::string_view key, int64_t value);
        LogBuilder& field(std::string_view key, double value);
        LogBuilder& field(std::string_view key, bool value);

        void log(LogLevel level, std::string_view msg,
                 std::source_location loc = std::source_location::current());

    private:
        friend class Logger;
        Logger* logger_;
        std::vector<LogField> fields_;
    };

    LogBuilder with_fields() { return LogBuilder{this}; }

    // ── 配置 ──
    void set_level(LogLevel level);
    void add_sink(std::unique_ptr<LogSink> sink);
    void remove_sinks();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Metrics: 计数器 ──
class Counter {
public:
    explicit Counter(std::string_view name, std::string_view help = "");
    void increment(int64_t value = 1) noexcept;
    int64_t value() const noexcept;
    void reset() noexcept;
private:
    std::string name_;
    std::atomic<int64_t> value_;
};

// ── Metrics: 直方图 ──
class Histogram {
public:
    Histogram(std::string_view name,
              std::vector<double> boundaries,
              std::string_view help = "");
    void observe(double value) noexcept;
    struct HistogramSummary {
        uint64_t count;
        double sum;
        double min;
        double max;
        double p50, p90, p95, p99;
    };
    HistogramSummary summary() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Metrics: 仪表盘 ──
class Gauge {
public:
    explicit Gauge(std::string_view name, std::string_view help = "");
    void set(double value) noexcept;
    void increment(double value = 1.0) noexcept;
    void decrement(double value = 1.0) noexcept;
    double value() const noexcept;
private:
    std::string name_;
    std::atomic<double> value_;
};

// ── Metrics 注册表 ──
class MetricsRegistry {
public:
    static MetricsRegistry& instance();

    Counter& counter(std::string_view name, std::string_view help = "");
    Histogram& histogram(std::string_view name,
                          std::vector<double> boundaries,
                          std::string_view help = "");
    Gauge& gauge(std::string_view name, std::string_view help = "");

    // ── 导出 ──
    std::string to_json() const;        // Prometheus remote-write JSON
    std::string to_prometheus() const;  // Prometheus exposition format

private:
    MetricsRegistry() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── 全局日志器 ──
Logger& default_logger();
void set_default_logger(std::unique_ptr<Logger> logger);

}  // namespace quant::infra

// ── 便捷宏 ──
#define QUANT_TRACE(...)  quant::infra::default_logger().trace(__VA_ARGS__)
#define QUANT_DEBUG(...)  quant::infra::default_logger().debug(__VA_ARGS__)
#define QUANT_INFO(...)   quant::infra::default_logger().info(__VA_ARGS__)
#define QUANT_WARN(...)   quant::infra::default_logger().warn(__VA_ARGS__)
#define QUANT_ERROR(...)  quant::infra::default_logger().error(__VA_ARGS__)
#define QUANT_FATAL(...)  quant::infra::default_logger().fatal(__VA_ARGS__)
```

### 数据流描述

```
业务代码
    │
    ├─ QUANT_INFO("订单提交: symbol={}", symbol);
    │   → Logger::info() → 构造 LogRecord
    │   → 写入环形缓冲区（lock-free SPSC）
    │   → return（非阻塞）
    │
    ├─ logger.with_fields()
    │       .field("symbol", "000001.SZ")
    │       .field("price", 12.34)
    │       .field("volume", 500)
    │       .log(LogLevel::INFO, "订单已提交");
    │   → 构造结构化 LogRecord
    │   → 写入环形缓冲区
    │
    └─ 后台刷盘线程
        ├── 批量取出 LogRecord[]
        ├── 序列化为 JSON（simdjson 或 fmt 库格式化）
        ├── 写入文件 / 控制台 / 网络端点
        └── 定期 flush

JSON 输出示例:
{
  "ts": "2026-05-15T09:30:00.123456789+08:00",
  "level": "INFO",
  "msg": "订单已提交",
  "loc": "order_manager.cpp:142",
  "thread": "trading-0",
  "symbol": "000001.SZ",
  "price": 12.34,
  "volume": 500
}
```

### 线程模型与并发策略

| 要素 | 策略 |
|------|------|
| 写入缓冲 | 环形缓冲区（SPSC lock-free queue），每线程一个缓冲区以去锁 |
| 缓冲合并 | 后台刷盘线程定时（1ms）从各 thread-local 缓冲区批量收割 |
| 刷盘 | 后台线程批量写文件，`writev` 系统调用批量提交 |
| 级别过滤 | 写入前快速 `if (level < min_level_) return;`，避免构造开销 |
| FATAL 处理 | 同步刷盘后 `abort()`，确保关键日志不丢失 |
| Metrics | 原子操作 + 分桶，无锁直方图使用 `HdrHistogram` 算法 |

### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| 热路径写入延迟 | < 200 ns（INFO 及以上级别） |
| TRACE 级别（关闭时） | < 5 ns（branch prediction 零开销） |
| 异步刷盘吞吐 | > 1M records/s |
| 结构化 JSON 序列化 | < 500 ns/条 |
| Metrics 计数器增量 | < 10 ns（atomic fetch_add） |
| Metrics 直方图 observe | < 100 ns |

---

## 4. 网络层

### 核心设计

基于 io_uring（Linux ≥ 5.1）或 epoll 的异步网络框架，外部封装 WebSocket 服务器供前端/飞书机器人接入，内部封装券商 TCP/UDP 协议对接框架。

### 关键接口

```cpp
// event_loop.h — 事件循环抽象
#pragma once

#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <span>

namespace quant::infra::net {

// ── IO 后端选择 ──
enum class IOBackend {
    IOUring,   // Linux io_uring（优先）
    Epoll,     // Linux epoll（fallback）
};

// ── IO 事件 ──
struct IOEvent {
    int fd;
    uint32_t events;    // EPOLLIN / EPOLLOUT / ...
    uint64_t user_data;
};

// ── 完成事件 ──
struct Completion {
    int result;         // >= 0 成功字节数, < 0 错误码
    uint64_t user_data;
};

// ── 事件循环 ──
class EventLoop {
public:
    explicit EventLoop(IOBackend backend = IOBackend::IOUring,
                         size_t queue_depth = 1024);
    ~EventLoop();

    // ── 异步 IO 操作 ──
    void submit_accept(int listen_fd, uint64_t user_data);
    void submit_recv(int fd, std::span<std::byte> buffer, uint64_t user_data);
    void submit_send(int fd, std::span<const std::byte> data, uint64_t user_data);
    void submit_connect(int fd, const struct sockaddr* addr, socklen_t addrlen,
                         uint64_t user_data);
    void submit_close(int fd, uint64_t user_data);

    // ── 定时器 ──
    uint64_t submit_timer(std::chrono::nanoseconds delay, uint64_t user_data);
    void cancel_timer(uint64_t timer_id);

    // ── 事件循环 ──
    void run();                               // 阻塞运行
    void stop() noexcept;                      // 优雅停止
    bool poll(std::chrono::nanoseconds timeout); // 单次轮询

    // ── 完成回调 ──
    using CompletionHandler = std::function<void(const Completion&)>;
    void set_completion_handler(CompletionHandler handler);

    // ── 协程支持 ──
    class Awaitable {
    public:
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept;
        Completion await_resume() noexcept;
    };
    Awaitable co_recv(int fd, std::span<std::byte> buffer);
    Awaitable co_send(int fd, std::span<const std::byte> data);
    Awaitable co_accept(int listen_fd);
    Awaitable co_connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
    Awaitable co_timer(std::chrono::nanoseconds delay);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quant::infra::net
```

```cpp
// websocket_server.h — WebSocket 服务器
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace quant::infra::net {

// ── WebSocket 帧类型 ──
enum class WSOpcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

// ── WebSocket 消息 ──
struct WSMessage {
    uint64_t connection_id;
    WSOpcode opcode;
    std::span<const std::byte> payload;
};

// ── WebSocket 服务器配置 ──
struct WSServerConfig {
    std::string listen_addr = "0.0.0.0";
    uint16_t port = 8080;
    uint16_t thread_count = 2;          // IO 线程数
    size_t max_connections = 10000;
    size_t max_message_size = 1 << 20;  // 1MB
    std::chrono::seconds idle_timeout{300};
    std::chrono::seconds ping_interval{30};
    bool enable_ssl = false;
    std::string cert_file;
    std::string key_file;
};

// ── WebSocket 服务器 ──
class WebSocketServer {
public:
    explicit WebSocketServer(const WSServerConfig& cfg);
    ~WebSocketServer();

    // ── 生命周期 ──
    Result<void> start();
    void stop() noexcept;

    // ── 回调 ──
    using OnConnect = std::function<void(uint64_t conn_id)>;
    using OnMessage = std::function<void(uint64_t conn_id, WSMessage msg)>;
    using OnDisconnect = std::function<void(uint64_t conn_id, std::string_view reason)>;

    void on_connect(OnConnect cb);
    void on_message(OnMessage cb);
    void on_disconnect(OnDisconnect cb);

    // ── 广播 / 定向发送 ──
    void broadcast(std::string_view channel, std::span<const std::byte> data);
    void send(uint64_t conn_id, std::span<const std::byte> data, WSOpcode opcode = WSOpcode::Binary);
    void close(uint64_t conn_id, uint16_t code = 1000, std::string_view reason = "");

    // ── 统计 ──
    struct Stats {
        uint64_t total_connections;
        uint64_t active_connections;
        uint64_t messages_received;
        uint64_t messages_sent;
        uint64_t bytes_received;
        uint64_t bytes_sent;
    };
    Stats stats() const noexcept;

    // ── 频道订阅 ──
    void subscribe(uint64_t conn_id, std::string_view channel);
    void unsubscribe(uint64_id, std::string_view channel);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quant::infra::net
```

```cpp
// broker_connection.h — 券商协议对接框架
#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace quant::infra::net {

// ── 连接状态 ──
enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Authenticating,
    Authenticated,
    Reconnecting,
};

// ── 连接配置 ──
struct BrokerConnectionConfig {
    std::string name;                  // 券商标识
    std::string host;
    uint16_t port;
    bool use_tls = false;
    std::string username;
    std::string password;
    std::string license_key;           // 券商授权码

    std::chrono::seconds heartbeat_interval{30};
    std::chrono::seconds reconnect_interval{5};
    int max_reconnect_attempts = 10;

    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds read_timeout{5};

    // 协议类型
    enum class Protocol { TCP, UDP };
    Protocol protocol = Protocol::TCP;
};

// ── 消息编解码接口 ──
class BrokerCodec {
public:
    virtual ~BrokerCodec() = default;

    // ── 编码 ──
    virtual std::vector<std::byte> encode_login(const BrokerConnectionConfig& cfg) = 0;
    virtual std::vector<std::byte> encode_logout() = 0;
    virtual std::vector<std::byte> encode_heartbeat() = 0;

    // ── 解码原始字节流 ──
    // 返回: 消费了多少字节 + 解码出的消息列表
    virtual std::pair<size_t, std::vector<BrokerMessage>>
        decode(std::span<const std::byte> data) = 0;
};

// ── 券商消息（通用表示） ──
struct BrokerMessage {
    enum Type {
        LoginResponse,
        LogoutResponse,
        HeartbeatAck,
        MarketData,
        OrderAck,
        OrderReject,
        FillReport,
        PositionUpdate,
        Custom,
    };
    Type type;
    std::string msg_type;        // 原始消息类型名
    std::vector<std::byte> payload;
};

// ── 连接抽象 ──
class BrokerConnection {
public:
    virtual ~BrokerConnection() = default;

    // ── 生命周期 ──
    virtual Result<void> connect() = 0;
    virtual Result<void> disconnect() = 0;
    virtual Result<void> login() = 0;
    virtual Result<void> logout() = 0;

    // ── 发送 ──
    virtual Result<void> send(std::span<const std::byte> data) = 0;

    // ── 状态 ──
    virtual ConnectionState state() const noexcept = 0;
    virtual const BrokerConnectionConfig& config() const noexcept = 0;

    // ── 回调 ──
    using OnMessage = std::function<void(const BrokerMessage&)>;
    using OnStateChange = std::function<void(ConnectionState, ConnectionState)>;

    virtual void on_message(OnMessage cb) = 0;
    virtual void on_state_change(OnStateChange cb) = 0;

    // ── 心跳 ──
    virtual void start_heartbeat() = 0;
    virtual void stop_heartbeat() = 0;
};

// ── 券商连接工厂 ──
class BrokerConnectionFactory {
public:
    // 注册编解码器
    static void register_codec(std::string_view broker_name,
                                std::unique_ptr<BrokerCodec> codec);

    // 创建连接
    static std::unique_ptr<BrokerConnection> create(
        const BrokerConnectionConfig& cfg,
        EventLoop* loop = nullptr  // 可选共享事件循环
    );
};

}  // namespace quant::infra::net
```

### 数据流描述

```
                        WebSocket 数据流
                               │
                               ▼
┌──────────────────────────────────────────────────────┐
│                WebSocketServer                       │
│  ┌─────────┐  ┌─────────┐  ┌────────────────────┐   │
│  │ IO #0   │  │ IO #1   │  │  Channel Manager    │   │
│  │(io_uring)│  │(io_uring)│  │  (subscribe/pub)   │   │
│  └────┬────┘  └────┬────┘  └────────┬───────────┘   │
│       │             │                │               │
│       └──────┬──────┘                │               │
│              ▼                       │               │
│       ┌──────────────┐               │               │
│       │  Callback    │◄──────────────┘               │
│       │  Router      │                               │
│       └──────┬───────┘                               │
└──────────────┼───────────────────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────────┐
│         业务层（策略/风控/行情）              │
└──────────────────────────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────────┐
│       BrokerConnection / Codec                │
│  ┌───────────────┐    ┌───────────────────┐  │
│  │ Heartbeat     │    │ Reconnect Manager │  │
│  │ Timer         │    │ (exponential back- │  │
│  │ (io_uring)    │    │  off retry)        │  │
│  └───────────────┘    └───────────────────┘  │
│                                              │
│  ┌───────────────────────────────────────┐   │
│  │ TCP/UDP Connection (via EventLoop)    │   │
│  └───────────────────────────────────────┘   │
└──────────────────────────────────────────────┘
               │
               ▼
          券商网关
```

### 线程模型与并发策略

| 要素 | 策略 |
|------|------|
| IO 线程 | 可配置数量（默认 2），每个 io_uring 实例绑定一个 CPU 核心 |
| 连接分配 | 按 `connection_id % io_thread_count` 分配到 IO 线程 |
| 心跳 | io_uring 定时器，超时自动发送 |
| 重连 | 指数退避（5s → 10s → 20s → ... → max），最多 10 次 |
| 消息解码 | IO 线程解码，业务回调投递到 ThreadPool |
| 广播 | sharded 写缓冲区，避免全局锁 |

### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| WebSocket 连接容量 | 10,000 并发 |
| WebSocket 消息吞吐 | > 500K msg/s |
| WebSocket 消息延迟（P99） | < 1 ms |
| 券商连接发送延迟 | < 100 μs |
| 券商连接接收延迟 | < 50 μs |
| 券商连接断线重连时间 | < 30s（含退避） |

---

## 5. C++/Python 互操作层

### 核心设计

使用 pybind11 将 C++ 核心计算引擎暴露给 Python。数据传输优先使用零拷贝（共享内存 + Arrow 格式），异步调用通过 `std::future` / `asyncio.Future` 桥接。

### 关键接口

```cpp
// pybind/bindings.h — pybind11 绑定组织头文件
#pragma once

// 绑定模块组织：
// quant._core          → 基础设施（线程池、配置、日志）
// quant._data          → 数据层（行情、因子）
// quant._strategy      → 策略引擎
// quant._backtest      → 回测引擎
// quant._risk          → 风控引擎

namespace quant::pybind {

// ── 共享内存缓冲区（零拷贝核心） ──
class SharedBuffer {
public:
    // 从共享内存创建
    static Result<SharedBuffer> create(std::string_view name, size_t size);

    // 打开已有共享内存
    static Result<SharedBuffer> open(std::string_view name);

    // 访问数据
    std::span<const std::byte> data() const noexcept;
    std::span<std::byte> mutable_data() noexcept;
    size_t size() const noexcept;

    // 转为 Arrow RecordBatch（零拷贝）
    Result<std::shared_ptr<arrow::RecordBatch>> to_arrow() const;

    // 从 Arrow RecordBatch 创建（零拷贝）
    static Result<SharedBuffer> from_arrow(
        const std::shared_ptr<arrow::RecordBatch>& batch);

    // 引用计数（跨进程安全）
    void add_ref() noexcept;
    void release() noexcept;
    int32_t ref_count() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// ── 异步桥接：C++ future → Python awaitable ──
template<typename T>
class PyAwaitable {
public:
    explicit PyAwaitable(std::future<T>&& future) : future_(std::move(future)) {}

    // Python __await__ protocol
    bool await_ready();
    void await_suspend(std::coroutine_handle<> handle);
    T await_resume();

private:
    std::future<T> future_;
    std::optional<T> result_;
};

// ── 类型映射注册表 ──
class TypeMapper {
public:
    // 注册 C++ → Python 类型映射
    template<typename CppType, typename PyType>
    static void register_type();

    // 注册错误码 → Python 异常映射
    static void register_exception(ErrorCode code,
                                    pybind11::object py_exception);

    // 转换函数
    static pybind11::object to_python(const std::any& cpp_value);
    static std::any from_python(pybind11::handle py_value);
};

// ── GIL 管理工具 ──
class GILScopedRelease {
public:
    GILScopedRelease() {PyEval_SaveThread();}
    ~GILScopedRelease() {PyEval_RestoreThread(saved_);}
private:
    PyThreadState* saved_;
};

class GILScopedAcquire {
public:
    GILScopedAcquire() { state_ = PyGILState_Ensure(); }
    ~GILScopedAcquire() { PyGILState_Release(state_); }
private:
    PyGILState_STATE state_;
};

}  // namespace quant::pybind
```

```python
# quant/_core.pyi — Python 侧类型存根

from typing import Any, Awaitable, Generic, TypeVar
import numpy as np

T = TypeVar("T")

class SharedBuffer:
    """零拷贝共享内存缓冲区"""
    @staticmethod
    def create(name: str, size: int) -> SharedBuffer: ...
    @staticmethod
    def open(name: str) -> SharedBuffer: ...
    def to_numpy(self) -> np.ndarray: ...
    def to_arrow(self) -> Any: ...  # pyarrow.RecordBatch

class PyAwaitable(Generic[T]):
    """异步桥接：可 await 的 C++ future"""
    def __await__(self) -> Any: ...

class ThreadPool:
    def submit(self, func: Any) -> PyAwaitable[Any]: ...
    def co_submit(self, func: Any) -> PyAwaitable[Any]: ...

class ConfigManager:
    def get(self, key: str, default: Any = None) -> Any: ...
    def set(self, key: str, value: Any, level: str = "runtime") -> None: ...
    def subscribe(self, key_pattern: str, callback: Any) -> int: ...
```

### 数据流描述

```
Python 调用路径：
───────────────────────────────────────────────────
│  Python                    C++                   │
│                                                  │
│  result = await engine.compute(data)             │
│       │                                          │
│       ├─ PyAwaitable.__await__()                  │
│       │   └─ future_ = engine.compute(data)       │
│       │      └─ C++ ThreadPool 执行               │
│       │         └─ 释放 GIL (GILScopedRelease)  │
│       │                                          │
│       │  ... C++ 计算中（GIL 已释放）...          │
│       │                                          │
│       ├─ 计算完成                                  │
│       │   └─ 获取 GIL (GILScopedAcquire)         │
│       │   └─ result_ = future_.get()              │
│       │   └─ 恢复 Python 协程                     │
│       ▼                                          │
│  result  ←  转换为 Python 对象                     │
│                                                  │
│  大数据零拷贝路径：                                │
│                                                  │
│  buffer = SharedBuffer.create("factor_data")     │
│       │                                          │
│       ├─ C++ 侧写入 Arrow IPC 到共享内存          │
│       │                                          │
│  df = buffer.to_arrow()  ← 零拷贝映射            │
│  # 或 buffer.to_numpy()  ← 零拷贝映射            │
───────────────────────────────────────────────────
```

### 线程模型与并发策略

| 要素 | 策略 |
|------|------|
| GIL | C++ 计算密集阶段释放 GIL（`GILScopedRelease`），回调时重新获取 |
| 数据传输 | 小数据用 pybind11 直接转换；大数据用共享内存 + Arrow IPC 零拷贝 |
| 异步桥接 | `std::future<T>` → `PyAwaitable<T>` → Python `await` |
| 错误传递 | `ErrorCode` → 映射为 Python 异常类，保留错误链 |
| 类型转换 | 内置类型的自动映射；`std::vector` ↔ `numpy.ndarray`；`std::string` ↔ `str` |

### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| 小对象跨语言调用延迟 | < 5 μs |
| 零拷贝大数据传输延迟 | < 1 μs（映射开销） |
| GIL 持有时间 | < 1% 总计算时间 |
| 异步调用 round-trip | < 50 μs（含线程池调度） |
| 类型转换开销（小对象） | < 500 ns |

---

## 6. 调度引擎

### 核心设计

基于 DAG 的定时任务调度引擎，支持任务依赖管理、故障恢复（重试/跳过/告警），预留分布式调度接口。

### 关键接口

```cpp
// scheduler.h
#pragma once

#include <chrono>
#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace quant::infra::scheduler {

// ── 任务 ID ──
using TaskId = std::string;

// ── 任务状态 ──
enum class TaskState {
    Pending,
    Running,
    Success,
    Failed,
    Skipped,
    Retrying,
    Cancelled,
};

// ── 调度策略 ──
struct ScheduleCron {
    std::string expression;  // 标准 cron 表达式
};

struct ScheduleInterval {
    std::chrono::seconds interval;
    std::optional<std::chrono::seconds> initial_delay;
};

struct ScheduleOnce {
    std::chrono::system_clock::time_point at;
};

struct ScheduleEventDriven {
    std::string event_type;   // e.g. "market_open", "data_updated"
};

using SchedulePolicy = std::variant<ScheduleCron, ScheduleInterval, ScheduleOnce, ScheduleEventDriven>;

// ── 重试策略 ──
struct RetryPolicy {
    int max_retries = 3;
    std::chrono::seconds initial_backoff{5};
    double backoff_multiplier = 2.0;
    std::chrono::seconds max_backoff{300};
    std::set<ErrorCode> retryable_errors;  // 可重试的错误码集合
};

// ── 任务结果 ──
struct TaskResult {
    TaskId task_id;
    TaskState state;
    int64_t exit_code = 0;
    std::string message;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    std::optional<std::unique_ptr<ErrorNode>> error;
};

// ── 任务概念 ──
template<typename T>
concept TaskFunction = std::is_invocable_r_v<Result<void>, T>;

// ── 任务定义 ──
class Task {
public:
    Task(TaskId id, std::function<Result<void>()> func);

    // ── 配置 ──
    Task& schedule(SchedulePolicy policy);
    Task& depends_on(std::initializer_list<TaskId> deps);
    Task& retry(RetryPolicy policy);
    Task& timeout(std::chrono::seconds duration);
    Task& on_failure(std::function<void(const TaskResult&)> callback);
    Task& tag(std::string_view tag);

    // ── 访问 ──
    const TaskId& id() const noexcept;
    const std::set<TaskId>& dependencies() const noexcept;
    const SchedulePolicy& schedule_policy() const noexcept;
    const RetryPolicy& retry_policy() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── DAG 依赖图 ──
class DependencyGraph {
public:
    // 添加任务及依赖
    Result<void> add_task(Task task);
    Result<void> add_dependency(const TaskId& from, const TaskId& to);

    // 拓扑排序（检测环）
    Result<std::vector<TaskId>> topological_sort() const;

    // 查询
    std::vector<TaskId> ready_tasks(const std::set<TaskId>& completed) const;
    std::vector<TaskId> dependents_of(const TaskId& task_id) const;
    bool has_cycle() const;

    // 可视化
    std::string to_dot() const;  // 生成 Graphviz DOT

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── 调度事件 ──
struct SchedulerEvent {
    enum Type {
        TaskStarted,
        TaskCompleted,
        TaskFailed,
        TaskRetryScheduled,
        TaskSkipped,
        SchedulerStarted,
        SchedulerStopped,
    };
    Type type;
    TaskId task_id;
    TaskResult result;
    std::chrono::system_clock::time_point timestamp;
};

// ── 事件监听器 ──
using SchedulerListener = std::function<void(const SchedulerEvent&)>;

// ── 调度器 ──
class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    // ── 注册任务 ──
    Result<void> register_task(Task task);
    Result<void> register_tasks(std::vector<Task> tasks);

    // ── 生命周期 ──
    Result<void> start();
    void stop() noexcept;
    void pause();
    void resume();

    // ── 触发 ──
    Result<void> trigger(const TaskId& task_id);              // 手动触发
    Result<void> trigger_event(std::string_view event_type);  // 事件触发
    Result<void> trigger_all();                                // 触发所有待运行任务

    // ── 查询 ──
    TaskState task_state(const TaskId& task_id) const;
    const TaskResult* task_result(const TaskId& task_id) const;
    std::vector<TaskId> pending_tasks() const;
    std::vector<TaskId> running_tasks() const;

    // ── 事件订阅 ──
    uint64_t subscribe(SchedulerListener listener);
    void unsubscribe(uint64_t id);

    // ── 统计 ──
    struct Stats {
        uint64_t total_scheduled;
        uint64_t total_completed;
        uint64_t total_failed;
        uint64_t total_retried;
        uint64_t total_skipped;
        std::chrono::seconds uptime;
    };
    Stats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── 分布式调度接口（预留） ──
class DistributedSchedulerBackend {
public:
    virtual ~DistributedSchedulerBackend() = default;

    // ── 分布式锁 ──
    virtual Result<void> acquire_lock(const TaskId& task_id,
                                       std::chrono::seconds ttl) = 0;
    virtual Result<void> release_lock(const TaskId& task_id) = 0;

    // ── 任务状态存储 ──
    virtual Result<void> save_task_state(const TaskId& task_id,
                                          const TaskResult& result) = 0;
    virtual Result<TaskResult> load_task_state(const TaskId& task_id) = 0;

    // ── 事件广播 ──
    virtual Result<void> broadcast_event(const SchedulerEvent& event) = 0;
    virtual Result<void> subscribe_events(const std::string& channel,
                                            SchedulerListener listener) = 0;

    // ── 心跳 ──
    virtual Result<void> heartbeat(const std::string& node_id) = 0;
};

}  // namespace quant::infra::scheduler
```

### 数据流描述

```
                      调度引擎数据流
                            │
      ┌─────────────────────┼──────────────────────┐
      │                     │                      │
      ▼                     ▼                      ▼
ScheduleCron          ScheduleInterval        ScheduleEventDriven
(每天09:25)           (每5分钟)               (market_open 事件)
      │                     │                      │
      └─────────────────────┼──────────────────────┘
                            │
                            ▼
                    ┌───────────────────┐
                    │   Timer Wheel /   │
                    │   Cron Parser     │
                    └────────┬──────────┘
                             │ trigger
                             ▼
                    ┌───────────────────┐
                    │  DependencyGraph  │
                    │  (DAG Topology)   │
                    └────────┬──────────┘
                             │ ready_tasks(completed)
                             ▼
                    ┌───────────────────┐
                    │  Task Executor    │
                    │  (ThreadPool)     │
                    └────────┬──────────┘
                             │
                    ┌────────┼────────┐
                    │        │        │
                    ▼        ▼        ▼
                 Success   Failed   Timeout
                    │        │        │
                    │      retry?   retry?
                    │     ┌──┴──┐   │
                    │   YES    NO   YES
                    │     │     │     │
                    │     ▼     ▼     ▼
                    │  backoff skip  backoff
                    │  + alert  + alert + alert
                    │     │     │     │
                    └─────┴─────┴─────┘
                             │
                             ▼
                    ┌───────────────────┐
                    │  Event Listeners  │
                    │  (通知/告警/日志)  │
                    └───────────────────┘
```

### 线程模型与并发策略

| 要素 | 策略 |
|------|------|
| 调度线程 | 1 个定时器线程（timer wheel），负责触发到时任务 |
| 执行线程 | 复用 `ThreadPool`，任务执行与调度分离 |
| DAG 解析 | 注册时静态验证（检测环路），运行时动态计算就绪集合 |
| 重试 | 失败后按 `backoff_multiplier` 指数退避重新入队 |
| 事件通知 | 回调在调度线程执行；耗时操作应投递到 ThreadPool |
| 分布式 | 预留 `DistributedSchedulerBackend` 接口，单机模式使用本地实现 |

### 性能指标目标

| 指标 | 目标值 |
|------|--------|
| 任务调度精度 | < 100 ms 偏差 |
| DAG 就绪集计算 | < 1 ms（1000 节点 DAG） |
| 任务注册延迟 | < 100 μs |
| 事件通知延迟 | < 1 ms |
| 重试退避调度 | 基于定时精度，误差 < 5% |

---

## 7. 目录结构与文件组织

遵循 Google C++ Style Guide 的项目布局，使用 Bazel 构建系统。

```
quant_invest/
├── WORKSPACE.bazel                      # Bazel 工作区
├── .bazelrc                             # Bazel 配置
├── .clang-format                        # 格式化配置
├── .clang-tidy                          # 静态分析配置
│
├── cpp/                                 # C++ 根目录
│   ├── quant/                           # 命名空间 quant::
│   │   ├── infra/                       # 命名空间 quant::infra
│   │   │   ├── thread_pool.h            # 线程池
│   │   │   ├── thread_pool.cc
│   │   │   ├── memory_pool.h            # 内存池
│   │   │   ├── memory_pool.cc
│   │   │   ├── object_pool.h            # 对象池
│   │   │   ├── object_pool.cc
│   │   │   ├── time_utils.h             # 时间工具
│   │   │   ├── time_utils.cc
│   │   │   ├── error_codes.h            # 错误码
│   │   │   ├── error_codes.cc
│   │   │   ├── config/                  # 配置管理
│   │   │   │   ├── config_manager.h
│   │   │   │   ├── config_manager.cc
│   │   │   │   ├── config_source.h
│   │   │   │   ├── yaml_source.h
│   │   │   │   ├── yaml_source.cc
│   │   │   │   ├── toml_source.h
│   │   │   │   ├── toml_source.cc
│   │   │   │   └── config_validator.h
│   │   │   ├── logging/                 # 日志系统
│   │   │   │   ├── logger.h
│   │   │   │   ├── logger.cc
│   │   │   │   ├── log_sink.h
│   │   │   │   ├── json_sink.h
│   │   │   │   ├── json_sink.cc
│   │   │   │   ├── console_sink.h
│   │   │   │   ├── console_sink.cc
│   │   │   │   ├── metrics.h
│   │   │   │   └── metrics.cc
│   │   │   ├── net/                     # 网络层
│   │   │   │   ├── event_loop.h
│   │   │   │   ├── event_loop.cc
│   │   │   │   ├── io_uring_backend.h
│   │   │   │   ├── io_uring_backend.cc
│   │   │   │   ├── epoll_backend.h
│   │   │   │   ├── epoll_backend.cc
│   │   │   │   ├── websocket_server.h
│   │   │   │   ├── websocket_server.cc
│   │   │   │   ├── broker_connection.h
│   │   │   │   ├── broker_connection.cc
│   │   │   │   └── connection_manager.h
│   │   │   ├── scheduler/               # 调度引擎
│   │   │   │   ├── scheduler.h
│   │   │   │   ├── scheduler.cc
│   │   │   │   ├── task.h
│   │   │   │   ├── dependency_graph.h
│   │   │   │   ├── dependency_graph.cc
│   │   │   │   ├── cron_parser.h
│   │   │   │   ├── cron_parser.cc
│   │   │   │   ├── timer_wheel.h
│   │   │   │   ├── timer_wheel.cc
│   │   │   │   └── distributed_backend.h
│   │   │   └── BUILD                    # Bazel 构建文件
│   │   │
│   │   ├── pybind/                      # C++/Python 互操作
│   │   │   ├── module_core.cc           # quant._core 绑定
│   │   │   ├── module_data.cc           # quant._data 绑定
│   │   │   ├── module_strategy.cc       # quant._strategy 绑定
│   │   │   ├── shared_buffer.h          # 零拷贝缓冲区
│   │   │   ├── shared_buffer.cc
│   │   │   ├── py_awaitable.h           # 异步桥接
│   │   │   ├── type_mapper.h            # 类型映射
│   │   │   ├── type_mapper.cc
│   │   │   ├── gil_utils.h              # GIL 管理工具
│   │   │   └── BUILD
│   │   │
│   │   └── BUILD                        # 顶层 Bazel 目标
│   │
│   └── test/                            # 测试
│       ├── infra/
│       │   ├── thread_pool_test.cc
│       │   ├── memory_pool_test.cc
│       │   ├── object_pool_test.cc
│       │   ├── time_utils_test.cc
│       │   ├── error_codes_test.cc
│       │   ├── config_test.cc
│       │   ├── logging_test.cc
│       │   ├── metrics_test.cc
│       │   ├── event_loop_test.cc
│       │   ├── websocket_test.cc
│       │   ├── broker_connection_test.cc
│       │   ├── scheduler_test.cc
│       │   └── dependency_graph_test.cc
│       ├── pybind/
│       │   ├── shared_buffer_test.cc
│       │   ├── py_awaitable_test.cc
│       │   └── type_mapper_test.cc
│       └── BUILD
│
├── python/                              # Python 包目录
│   └── quant/
│       ├── __init__.py
│       ├── _core.pyi                    # 类型存根
│       ├── config.py                    # 配置管理 Python 封装
│       └── ...
│
├── configs/                             # 配置文件
│   ├── global.yaml                      # 全局配置
│   ├── data.yaml                        # 数据模块配置
│   ├── trading.yaml                     # 交易模块配置
│   └── strategies/                      # 策略配置
│       └── ma_cross.yaml
│
├── docs/                                # 文档
│   ├── architecture_cpp_infra.md        # 本文档
│   └── architecture_frontend.md          # 前端架构
│
└── third_party/                         # 第三方依赖
    ├── yaml-cpp/
    ├── toml11/
    ├── pybind11/
    ├── arrow/
    ├── fmt/
    └── ...
```

### 依赖关系与构建规则

```
┌──────────────────────────────────────────────────────────┐
│                       应用层                              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │  策略    │ │  回测    │ │   交易    │ │   风控    │  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘  │
└────────────────────────┬─────────────────────────────────┘
                         │
┌────────────────────────┼─────────────────────────────────┐
│                  基础组件层                                │
│                         │                                 │
│  ┌──────────┐ ┌────────┴─────┐ ┌──────────┐            │
│  │   日志   │ │   调度引擎   │ │  配置管理 │            │
│  └────┬─────┘ └──────┬───────┘ └────┬─────┘            │
│       │               │               │                   │
│  ┌────┴───────────────┴───────────────┴─────┐            │
│  │         公共基础库                        │            │
│  │  (ThreadPool, MemoryPool, ObjectPool,    │            │
│  │   TimeUtils, ErrorCodes)                 │            │
│  └────┬──────────────┬──────────────────────┘            │
│       │              │                                    │
│  ┌────┴────┐   ┌─────┴──────┐                           │
│  │  网络层  │   │ C++/Python │                           │
│  │(io_uring│   │ 互操作层    │                           │
│  │ WS SDK) │   │(pybind11   │                           │
│  └─────────┘   │ Arrow)     │                           │
│                └────────────┘                            │
└──────────────────────────────────────────────────────────┘
```

### BUILD 文件示例

```python
# cpp/quant/infra/BUILD
load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "infra",
    srcs = [
        "thread_pool.cc",
        "memory_pool.cc",
        "object_pool.cc",
        "time_utils.cc",
        "error_codes.cc",
    ],
    hdrs = [
        "thread_pool.h",
        "memory_pool.h",
        "object_pool.h",
        "time_utils.h",
        "error_codes.h",
    ],
    deps = [
        "@boost//lockfree",
        "@fmt//fmt",
        "@moodycamel//concurrentqueue",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "config",
    srcs = [
        "config/config_manager.cc",
        "config/yaml_source.cc",
        "config/toml_source.cc",
    ],
    hdrs = [
        "config/config_manager.h",
        "config/config_source.h",
        "config/yaml_source.h",
        "config/toml_source.h",
        "config/config_validator.h",
    ],
    deps = [
        ":infra",
        "@yaml_cpp//:yaml_cpp",
        "@toml11//:toml11",
    ],
    copts = ["-std=c++20"],
)

cc_library(
    name = "logging",
    srcs = [
        "logging/logger.cc",
        "logging/json_sink.cc",
        "logging/console_sink.cc",
        "logging/metrics.cc",
    ],
    hdrs = [
        "logging/logger.h",
        "logging/log_sink.h",
        "logging/json_sink.h",
        "logging/console_sink.h",
        "logging/metrics.h",
    ],
    deps = [
        ":infra",
        "@fmt//fmt",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "infra_test",
    srcs = glob(["test/infra/*_test.cc"]),
    deps = [
        ":infra",
        ":config",
        ":logging",
        "@gtest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)
```

---

## 附录：关键技术选型

| 组件 | 选型 | 版本 | 理由 |
|------|------|------|------|
| 语言标准 | C++20 | GCC 13+ / Clang 17+ | concepts, coroutines, source_location, format |
| 构建 | Bazel 7+ | 7.x | 增量构建、依赖管理、多语言支持 |
| 日志格式化 | fmtlib | 10.x | 高性能、编译期格式检查、C++20 兼容 |
| 无锁队列 | boost::lockfree | 1.84+ | 线程池本地队列、无锁数据结构 |
| 并发队列 | moodycamel | 1.x | 高吞吐全局队列 |
| 序列化 | simdjson (解析) + fmt (输出) | latest | JSON 高速解析/输出 |
| YAML | yaml-cpp | 0.8+ | 成熟 YAML 读写库 |
| TOML | toml11 | 4.x | header-only、C++20 兼容 |
| io_uring | liburing | 2.x | Linux 异步 IO 原生支持 |
| WebSocket | 自研 (基于 io_uring) | — | 避免依赖重量级库 |
| Python绑定 | pybind11 | 2.12+ | 官方推荐、C++20 支持 |
| 零拷贝 | Apache Arrow | 16+ | 列式数据、跨语言共享内存 |
| 测试 | GoogleTest | 1.14+ | C++ 标准测试框架 |
|  Benchmark | Google Benchmark | 1.8+ | 微基准测试 |
| 内存分配 | jemalloc | 5.x | 可选替换系统 malloc，碎片更少 |