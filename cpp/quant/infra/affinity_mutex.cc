// affinity_mutex.cc -- Implementation of executor-routed Mutex
//
// The core mechanism:
//   unlock(): atomically transition state. If waiters exist, dequeue one
//             and route its continuation via executor.add_to_worker().
//             Remaining waiters are re-pushed atomically.

#include "cpp/quant/infra/affinity_mutex.h"
#include "cpp/quant/infra/work_stealing_executor.h"

#include <folly/coro/Task.h>

namespace quant::infra {

// ── Worker ID resolution ──
// Reuses the same mechanism as AffinityBaton (detail::get_current_worker_id).

size_t AffinityMutex::current_worker_id() {
    return detail::get_current_worker_id();
}

// ── co_scoped_lock(): acquire and return RAII guard ──

folly::coro::Task<AffinityMutex::AffinityMutexLock>
AffinityMutex::co_scoped_lock() {
    co_await co_lock();
    co_return AffinityMutexLock(*this);
}

// ── unlock(): release lock, wake next waiter via executor ──
//
// State transitions:
//   kLockedFlag -> 0                    (no waiters, just release)
//   (waiters|kLockedFlag) -> new_state  (dequeue one waiter, re-push rest)
//
// The key insight: we atomically exchange the state to drain all waiters,
// then re-push the remaining ones. This avoids races because the exchange
// is atomic and new waiters that arrive during our processing will be
// merged via CAS.

void AffinityMutex::unlock() {
    // Fast path: try to release the lock if no waiters
    uintptr_t expected = kLockedFlag;
    if (state_.compare_exchange_strong(
            expected, 0,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        // Successfully released. But a waiter might have enqueued between
        // our initial check and the CAS. In that case, expected would have
        // been updated to the new state (with waiters), and CAS would fail.
        // We fall through to the slow path.
        return;
    }

    // Slow path: there are waiters (or a waiter arrived during fast path).
    // Atomically drain the entire waiter stack.
    auto old_state = state_.exchange(kLockedFlag, std::memory_order_acq_rel);

    auto* waiters = extract_waiters(old_state);
    if (!waiters) {
        // No waiters after all. Release the lock.
        state_.store(0, std::memory_order_release);
        return;
    }

    // Pop one waiter from the stack (LIFO — top of stack gets the lock).
    auto* to_wake = waiters;
    auto* remaining = waiters->next;
    to_wake->next = nullptr;

    // Re-push remaining waiters (if any) using CAS to merge with
    // any new arrivals that came during our exchange.
    if (remaining) {
        auto cur = state_.load(std::memory_order_acquire);
        do {
            // Find the tail of the remaining list
            auto* tail = remaining;
            while (tail->next) {
                tail = tail->next;
            }
            // Link: remaining -> ... -> tail -> existing_waiters -> ...
            auto* existing = extract_waiters(cur);
            tail->next = existing;
        } while (!state_.compare_exchange_weak(
            cur,
            reinterpret_cast<uintptr_t>(remaining) | kLockedFlag,
            std::memory_order_release,
            std::memory_order_acquire));
    }

    // Route the woken waiter to its original worker thread
    auto handle = to_wake->handle;
    auto worker_id = to_wake->worker_id;

    // Get the WorkStealingExecutor from thread-local context.
    // This is set by WorkStealingExecutor during worker_loop().
    auto* ws_executor = WorkStealingExecutor::current_executor();

    if (ws_executor && worker_id != SIZE_MAX) {
        // Route to the waiter's original worker via executor
        ws_executor->add_to_worker(worker_id, [handle]() mutable {
            handle.resume();
        });
    } else {
        // No executor or no affinity: resume inline
        handle.resume();
    }
}

}  // namespace quant::infra
