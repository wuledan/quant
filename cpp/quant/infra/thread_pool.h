// thread_pool.h — Work-Stealing thread pool with C++20 coroutine support
//
// Key design:
//   - co_submit() returns PoolAwareAwaiter that resumes the coroutine
//     on the pool (NOT via detached thread like the old TaskAwaiter)
//   - enqueue_handle() allows any coroutine_handle to be resumed on
//     a pool worker thread
//   - Synchronous submit() is preserved for non-coroutine code
#pragma once

#include <atomic>
#include <concepts>
#include <coroutine>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace quant::infra {

// ── Task concept ──
template<typename F>
concept CallableTask = std::is_invocable_v<std::decay_t<F>>;

// ── Task type erasure ──
class Task {
public:
    Task() = default;

    template<CallableTask F>
    explicit Task(F&& fn)  // NOLINT
        : func_(std::forward<F>(fn)) {}

    void execute() noexcept {
        if (func_) {
            func_();
        }
    }

    explicit operator bool() const noexcept { return static_cast<bool>(func_); }

private:
    std::function<void()> func_;
};

// ── Schedule policy ──
enum class SchedulePolicy {
    RoundRobin,     // Round-robin assignment
    LeastLoaded,    // Least loaded first
    WorkStealing,   // Local queue + steal (default)
};

// ── Thread pool config ──
struct ThreadPoolConfig {
    uint32_t worker_count = 0;          // 0 = auto-detect std::thread::hardware_concurrency()
    uint32_t local_queue_capacity = 256;
    uint32_t steal_attempts = 4;        // Max steal retry count
    SchedulePolicy policy = SchedulePolicy::WorkStealing;
    std::string thread_name_prefix = "quant-pool";
};

// ── Shared state for coroutine task completion ──
template<typename T>
struct CoTaskState {
    std::optional<T> result;
    std::exception_ptr exception;
    std::coroutine_handle<> waiter{nullptr};
    std::atomic<bool> completed{false};
    std::atomic<bool> resumed{false};  // Ensures exactly one resume
};

template<>
struct CoTaskState<void> {
    bool void_completed = false;
    std::exception_ptr exception;
    std::coroutine_handle<> waiter{nullptr};
    std::atomic<bool> completed{false};
    std::atomic<bool> resumed{false};
};

// ── PoolAwareAwaiter: co_await result from pool, resume ON the pool ──
//
// Replaces the broken TaskAwaiter that spawned a detached thread per
// co_await. This awaiter:
//   1. Does NOT create any threads
//   2. When the submitted task completes, the coroutine handle is
//      enqueued back to the ThreadPool via enqueue_handle()
//   3. The coroutine resumes on a pool worker thread
template<typename T>
class PoolAwareAwaiter {
public:
    explicit PoolAwareAwaiter(std::shared_ptr<CoTaskState<T>> state,
                              class ThreadPool* pool)
        : state_(std::move(state)), pool_(pool) {}

    bool await_ready() const noexcept {
        return state_->completed.load(std::memory_order_acquire);
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept;

    T await_resume() {
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(*state_->result);
        }
    }

private:
    std::shared_ptr<CoTaskState<T>> state_;
    ThreadPool* pool_;
};

// ── Thread pool main class ──
class ThreadPool {
public:
    explicit ThreadPool(const ThreadPoolConfig& cfg = {});
    ~ThreadPool();

    // Disable copy
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // ── Synchronous submit ──
    template<CallableTask F>
    auto submit(F&& task) -> std::future<std::invoke_result_t<F>>;

    // ── Coroutine submit (replaces old co_submit with TaskAwaiter) ──
    // The returned PoolAwareAwaiter resumes the caller on a pool worker
    // when the task completes, WITHOUT creating any extra threads.
    template<CallableTask F>
    auto co_submit(F&& task) -> PoolAwareAwaiter<std::invoke_result_t<F>>;

    // ── Batch submit ──
    template<std::ranges::range R>
    auto submit_batch(R&& tasks) -> std::vector<std::future<void>>;

    // ── Enqueue a coroutine handle for resumption on a pool worker ──
    // This is the key primitive: instead of handle.resume() on a
    // detached thread, we enqueue the handle and a pool worker
    // calls handle.resume() within the pool's scheduling discipline.
    void enqueue_handle(std::coroutine_handle<> handle);

    // ── Lifecycle ──
    void start();
    void stop() noexcept;        // Wait for all tasks to complete
    void force_stop() noexcept;  // Abandon unexecuted tasks

    // ── Runtime stats ──
    struct Stats {
        uint64_t tasks_submitted;
        uint64_t tasks_completed;
        uint64_t tasks_stolen;
        uint64_t queue_overflow_count;
        uint64_t handles_resumed;  // NEW: coroutine handles resumed on pool
    };
    Stats stats() const noexcept;

    uint32_t worker_count() const noexcept;
    bool is_running() const noexcept;

    // ── Enqueue a type-erased task (used by submit template) ──
    void enqueue_task(Task task);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Global singleton (process-level default thread pool) ──
ThreadPool& default_thread_pool();

// ============================================================
// Template implementations
// ============================================================

template<CallableTask F>
auto ThreadPool::submit(F&& task) -> std::future<std::invoke_result_t<F>> {
    using ReturnType = std::invoke_result_t<F>;

    auto promise = std::make_shared<std::promise<ReturnType>>();
    auto future = promise->get_future();

    auto wrapper = [task = std::forward<F>(task), promise]() mutable {
        try {
            if constexpr (std::is_void_v<ReturnType>) {
                task();
                promise->set_value();
            } else {
                promise->set_value(task());
            }
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    };

    enqueue_task(Task(std::move(wrapper)));
    return future;
}

template<CallableTask F>
auto ThreadPool::co_submit(F&& task) -> PoolAwareAwaiter<std::invoke_result_t<F>> {
    using ReturnType = std::invoke_result_t<F>;
    auto state = std::make_shared<CoTaskState<ReturnType>>();
    auto* pool = this;

    auto wrapper = [task = std::forward<F>(task), state, pool]() mutable {
        try {
            if constexpr (std::is_void_v<ReturnType>) {
                task();
                state->void_completed = true;
            } else {
                state->result.emplace(task());
            }
        } catch (...) {
            state->exception = std::current_exception();
        }

        // Mark completed and attempt to resume the waiter
        state->completed.store(true, std::memory_order_release);

        if (state->waiter && !state->resumed.exchange(true, std::memory_order_acq_rel)) {
            pool->enqueue_handle(state->waiter);
        }
    };

    enqueue_task(Task(std::move(wrapper)));
    return PoolAwareAwaiter<ReturnType>(std::move(state), this);
}

template<std::ranges::range R>
auto ThreadPool::submit_batch(R&& tasks) -> std::vector<std::future<void>> {
    std::vector<std::future<void>> futures;
    futures.reserve(std::ranges::size(tasks));
    for (auto&& task : tasks) {
        futures.push_back(submit(std::forward<decltype(task)>(task)));
    }
    return futures;
}

// ── PoolAwareAwaiter::await_suspend ──
// Must be defined after ThreadPool is complete.
template<typename T>
void PoolAwareAwaiter<T>::await_suspend(std::coroutine_handle<> handle) noexcept {
    state_->waiter = handle;

    // If the task already completed (race with the wrapper), we
    // need to resume the coroutine ourselves.
    if (state_->completed.load(std::memory_order_acquire)) {
        if (!state_->resumed.exchange(true, std::memory_order_acq_rel)) {
            pool_->enqueue_handle(handle);
        }
        return;
    }

    // The wrapper will see state_->waiter and call enqueue_handle
    // when it completes.  No thread created, no blocking.
}

}  // namespace quant::infra
