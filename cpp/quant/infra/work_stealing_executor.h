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
#include "affinity_baton.h"

#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/coro/Baton.h>
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

    // ── Current executor (thread-local) ──
    // Returns the WorkStealingExecutor running on the current thread,
    // or nullptr if not on a pool worker thread.
    static WorkStealingExecutor* current_executor();

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
        // Concurrent MPMC queue for thread-affine external submissions.
        // Chase-Lev supports push/pop only from the owner thread (steal from
        // any) — add_to_worker / routing from non-owner threads MUST use this.
        using AffineQueue = folly::UMPMCQueue<WorkItem, false, 6>;
        std::unique_ptr<AffineQueue> affine_queue;
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
//
// Submits a callable to the executor and returns a Task that completes
// when the callable finishes. Uses AffinityBaton for coroutine signaling,
// which routes the resume via add_to_worker() back to the caller's worker
// thread — preserving thread affinity (design constraint 6).
//
// IMPORTANT: co_submit is designed to be called from within a coroutine
// context (co_await ex.co_submit(...)). Do NOT use folly::coro::blockingWait
// to bridge from synchronous code — use submit() for that instead.

template <typename F>
folly::coro::Task<std::invoke_result_t<F>>
WorkStealingExecutor::co_submit(F&& func) {
    using R = std::invoke_result_t<F>;

    struct SharedState {
        AffinityBaton baton;
        std::exception_ptr ex;
        std::conditional_t<std::is_void_v<R>, std::nullptr_t, R> result{};
    };
    auto state = std::make_shared<SharedState>();

    this->add([func = std::forward<F>(func), state, this]() mutable {
        try {
            if constexpr (std::is_void_v<R>) {
                func();
            } else {
                state->result = func();
            }
        } catch (...) {
            state->ex = std::current_exception();
        }
        // AffinityBaton::post(executor) drains the waiter chain and routes
        // each waiter's handle via add_to_worker(worker_id, handle),
        // which restores the coroutine on its original worker thread.
        state->baton.post(*this);
    });

    co_await state->baton;

    if (state->ex) {
        std::rethrow_exception(state->ex);
    }

    if constexpr (std::is_void_v<R>) {
        co_return;
    } else {
        co_return std::move(state->result);
    }
}


}  // namespace quant::infra
