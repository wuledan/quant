// thread_pool.h — Work-Stealing thread pool with C++20 coroutine support
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

// ── Awaiter: co_await task submission ──
template<typename T>
class TaskAwaiter {
public:
    explicit TaskAwaiter(std::future<T>&& future) : future_(std::move(future)) {}

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        // Launch a watcher thread that waits for the task result and
        // then resumes the calling coroutine on the same thread.
        // In a production system this would be integrated with an
        // event loop to avoid creating a thread per await.
        std::thread([this, handle]() mutable {
            future_.wait();
            handle.resume();
        }).detach();
    }

    T await_resume() {
        return future_.get();
    }

private:
    std::future<T> future_;
    std::coroutine_handle<> handle_;
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

    // ── Coroutine submit ──
    template<CallableTask F>
    auto co_submit(F&& task) -> TaskAwaiter<std::invoke_result_t<F>>;

    // ── Batch submit ──
    template<std::ranges::range R>
    auto submit_batch(R&& tasks) -> std::vector<std::future<void>>;

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
auto ThreadPool::co_submit(F&& task) -> TaskAwaiter<std::invoke_result_t<F>> {
    return TaskAwaiter<std::invoke_result_t<F>>(submit(std::forward<F>(task)));
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

}  // namespace quant::infra