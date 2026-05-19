// work_stealing_executor.h -- Work-stealing executor with thread affinity
//
// Implements folly::Executor with:
//   - Chase-Lev work-stealing deques (one per worker)
//   - Global MPMC queue for external submissions
//   - Thread affinity: co_submit/timer resume via add_to_worker
//   - Three-level backoff: spin -> yield -> futex park
//
// Design doc: docs/coroutine_refactor_dev_plan.md
#pragma once

#include "chase_lev_deque.h"

#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/coro/Task.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace quant::infra {

// ── WorkStealingExecutor ──
//
// Core scheduler for all coroutine tasks.
//
class WorkStealingExecutor : public folly::Executor {
public:
    using folly::Executor::add;

    // ── Construction / destruction ──
    explicit WorkStealingExecutor(
        size_t num_workers,
        std::string name_prefix = "quant-ws");
    ~WorkStealingExecutor() override;

    WorkStealingExecutor(const WorkStealingExecutor&) = delete;
    WorkStealingExecutor& operator=(const WorkStealingExecutor&) = delete;

    // ── folly::Executor interface ──
    void add(folly::Func func) override;

    // ── Thread-affine task submission ──
    void add_to_worker(size_t worker_id, folly::Func func);

    // ── Coroutine-based task submission with thread affinity ──
    template <typename F>
    folly::coro::Task<std::invoke_result_t<F>> co_submit(F&& func);

    // ── Lifecycle ──
    void start();
    void stop();
    void force_stop();

    // ── Query ──
    size_t worker_count() const noexcept { return num_workers_; }
    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // ── Worker ID (thread-local) ──
    static size_t current_worker_id();
    static bool is_pool_worker();
    static constexpr size_t kExternalThread = SIZE_MAX;

    // ── Statistics ──
    struct Stats {
        uint64_t tasks_submitted{0};
        uint64_t tasks_completed{0};
        uint64_t local_pops{0};
        uint64_t global_queue_pops{0};
        uint64_t local_steals{0};
        uint64_t failed_steals{0};
        uint64_t park_count{0};
        uint64_t unpark_count{0};
        uint64_t handles_resumed{0};
        double utilization{0.0};
        size_t active_workers{0};
    };

    Stats stats() const;

private:
    // ── Internal types ──

    struct WorkItem {
        folly::Func func;
        size_t target_worker_id{kExternalThread};
        bool is_coroutine_resume{false};

        void execute() noexcept {
            if (func) {
                func();
                func = nullptr;
            }
        }

        explicit operator bool() const noexcept {
            return static_cast<bool>(func);
        }
    };

    struct WorkerState {
        std::unique_ptr<ChaseLevDeque<WorkItem>> local_deque;
        std::thread thread;
        size_t worker_id;
        std::atomic<bool> parked{false};
        std::mutex park_mutex;
        std::condition_variable park_cv;

        // Three-level backoff
        uint32_t spin_count{0};
        uint32_t yield_count{0};

        // Utilization tracking (nanoseconds)
        std::atomic<uint64_t> busy_cycles_ns{0};
        std::atomic<uint64_t> idle_cycles_ns{0};

        // Event counters
        std::atomic<uint64_t> local_pops{0};
        std::atomic<uint64_t> steals_from_me{0};
        std::atomic<uint64_t> tasks_completed{0};
    };

    // ── Internal methods ──
    void worker_loop(size_t worker_id);
    void wake_one_worker();
    void wake_worker(size_t worker_id);

    // ── Constants ──
    static constexpr uint32_t kSpinMaxUs = 10;
    static constexpr uint32_t kYieldRounds = 3;
    static constexpr size_t kStealAttempts = 4;
    static constexpr size_t kLocalDequeCapacity = 256;
    static constexpr size_t kGlobalQueueCapacity = 8192;

    // ── Members ──
    std::vector<std::unique_ptr<WorkerState>> workers_;
    size_t num_workers_;
    std::string name_prefix_;

    folly::UMPMCQueue<WorkItem, false, 6> global_queue_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

};

// ── co_submit implementation (template, must be in header) ──

template <typename F>
folly::coro::Task<std::invoke_result_t<F>>
WorkStealingExecutor::co_submit(F&& func) {
    using R = std::invoke_result_t<F>;
    using Storage = std::conditional_t<std::is_void_v<R>, std::nullptr_t, R>;

    struct SharedState {
        std::coroutine_handle<> waiter;
        WorkStealingExecutor* executor;
        size_t caller_worker_id;
        std::exception_ptr ex;
        Storage result{};
        // 0 = running, 1 = completed, 2 = waiter suspended
        std::atomic<int> state{0};
    };

    auto shared = std::make_shared<SharedState>();
    shared->executor = this;
    shared->caller_worker_id = current_worker_id();

    this->add([func = std::forward<F>(func),
               shared]() mutable {
        if constexpr (std::is_void_v<R>) {
            try {
                func();
            } catch (...) {
                shared->ex = std::current_exception();
            }
        } else {
            try {
                shared->result = func();
            } catch (...) {
                shared->ex = std::current_exception();
            }
        }
        // Atomically mark completed; if waiter already suspended, route back
        int expected = 0;
        if (shared->state.compare_exchange_strong(expected, 1,
                std::memory_order_acq_rel)) {
            // Was running (0→1), waiter hasn't suspended yet — it will see 1
        } else {
            // Was 2 (waiter suspended) — we must resume it
            shared->executor->add_to_worker(
                shared->caller_worker_id,
                [shared]() { shared->waiter.resume(); });
        }
    });

    // Awaiter that handles the completion/suspension race correctly
    struct Awaiter {
        std::shared_ptr<SharedState> shared;

        bool await_ready() noexcept {
            return shared->state.load(std::memory_order_acquire) == 1;
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            shared->waiter = handle;
            auto s = shared;  // copy shared_ptr for lambda capture
            // Try to mark as suspended; if already completed, resume now
            int expected = 0;
            if (shared->state.compare_exchange_strong(expected, 2,
                    std::memory_order_acq_rel)) {
                // Successfully suspended (0→2), task will resume us
            } else {
                // Task already completed (state==1), resume immediately
                s->executor->add_to_worker(
                    s->caller_worker_id,
                    [s]() { s->waiter.resume(); });
            }
        }

        auto await_resume() -> std::conditional_t<std::is_void_v<R>, void, R> {
            if (shared->ex) {
                std::rethrow_exception(shared->ex);
            }
            if constexpr (!std::is_void_v<R>) {
                return std::move(shared->result);
            }
        }
    };

    if constexpr (std::is_void_v<R>) {
        co_await Awaiter{std::move(shared)};
        co_return;
    } else {
        co_return co_await Awaiter{std::move(shared)};
    }
}

}  // namespace quant::infra
