// affinity_shared_mutex.cc -- Implementation of executor-routed SharedMutex
//
// Wake-up strategy:
//   - unlock():       exclusive lock released. Try to wake waiters.
//   - unlock_shared(): shared lock released. If last reader and writer
//                      waiting, try to wake waiters.
//   - wake_waiters(): atomically drain the waiter stack. If the top waiter
//                      is a writer, wake just that writer (exclusive).
//                      If the top waiter is a reader, wake all consecutive
//                      readers from the stack top (batch wake) until a
//                      writer is encountered, then stop (writer blocks
//                      further reader wakes for fairness).
//   - Remaining waiters are re-pushed via CAS (merging with new arrivals).

#include "cpp/quant/infra/affinity_shared_mutex.h"
#include "cpp/quant/infra/work_stealing_executor.h"

#include <folly/coro/Task.h>

namespace quant::infra {

size_t AffinitySharedMutex::current_worker_id() {
    return detail::get_current_worker_id();
}

// ── co_scoped_lock() / co_scoped_shared_lock() ──

folly::coro::Task<AffinitySharedMutex::ExclusiveGuard>
AffinitySharedMutex::co_scoped_lock() {
    co_await co_lock();
    co_return ExclusiveGuard(*this);
}

folly::coro::Task<AffinitySharedMutex::SharedGuard>
AffinitySharedMutex::co_scoped_shared_lock() {
    co_await co_shared_lock();
    co_return SharedGuard(*this);
}

// ── unlock(): release exclusive lock ──

void AffinitySharedMutex::unlock() {
    // Clear kWriterLocked. May also clear kWriterWaiting if no waiters
    // remain, but we let wake_waiters() handle that.
    auto old_state = state_.fetch_sub(kWriterLocked, std::memory_order_acq_rel);

    // Sanity: we must have held the exclusive lock
    // (old_state & kWriterLocked) was set

    // If there are no readers and potentially waiters, try to wake them
    if (reader_count(old_state - kWriterLocked) == 0) {
        wake_waiters();
    }
}

// ── unlock_shared(): release shared lock ──

void AffinitySharedMutex::unlock_shared() {
    auto old_state = state_.fetch_sub(kReaderOne, std::memory_order_acq_rel);

    auto new_reader_count = reader_count(old_state - kReaderOne);

    // If this was the last reader and a writer is waiting, wake waiters
    if (new_reader_count == 0 && (old_state & kWriterWaiting)) {
        wake_waiters();
    }
}

// ── wake_waiters(): drain waiter stack and wake appropriate waiters ──
//
// Called when:
//   - unlock() released exclusive lock and no readers remain, OR
//   - unlock_shared() was the last reader and a writer is waiting
//
// Strategy:
//   1. Atomically drain the waiter stack
//   2. If top waiter is a writer: give exclusive lock, re-push rest
//   3. If top waiter is a reader: batch-wake consecutive readers, stop
//      at first writer (for fairness), re-push remaining
//   4. If no waiters: clear kWriterWaiting flag

void AffinitySharedMutex::wake_waiters() {
    // Atomically drain the waiter stack
    auto* waiters = waiters_.exchange(nullptr, std::memory_order_acq_rel);

    // Check for new arrivals that may have been pushed between our trigger
    // condition check and this exchange. We'll merge them after processing.
    // For now, process what we have.

    if (!waiters) {
        // No waiters. Clear kWriterWaiting if set.
        auto s = state_.load(std::memory_order_acquire);
        if (s & kWriterWaiting) {
            uint32_t expected = s;
            // Only clear kWriterWaiting if state hasn't changed
            state_.compare_exchange_strong(
                expected, s & ~kWriterWaiting,
                std::memory_order_release,
                std::memory_order_relaxed);
        }
        return;
    }

    // Reverse the LIFO stack to get FIFO order (fairer for writers)
    Waiter* reversed = nullptr;
    while (waiters) {
        auto* next = waiters->next;
        waiters->next = reversed;
        reversed = waiters;
        waiters = next;
    }
    // Now `reversed` is the FIFO-ordered list (oldest waiter first)

    // Process waiters from the front
    auto* to_wake_list = reversed;   // Waiters to wake
    Waiter* remaining = nullptr;     // Waiters to re-push

    if (to_wake_list) {
        if (to_wake_list->is_reader) {
            // Batch-wake consecutive readers from the front
            auto* last_reader = to_wake_list;
            auto count = 1u;
            while (last_reader->next && last_reader->next->is_reader) {
                last_reader = last_reader->next;
                ++count;
            }
            // last_reader->next is either nullptr or a writer
            remaining = last_reader->next;
            last_reader->next = nullptr;  // Detach reader list

            // Update state: add reader count, clear kWriterWaiting if no
            // writer in remaining list
            bool has_writer_waiting = false;
            for (auto* w = remaining; w; w = w->next) {
                if (!w->is_reader) { has_writer_waiting = true; break; }
            }

            auto new_state = state_.load(std::memory_order_acquire);
            // Clear kWriterLocked (was set by unlock), set reader count,
            // keep kWriterWaiting if there's a writer in remaining
            new_state = (count << kReaderShift)
                        | (has_writer_waiting ? kWriterWaiting : 0u);
            state_.store(new_state, std::memory_order_release);
        } else {
            // Top waiter is a writer: give exclusive lock
            auto* writer = to_wake_list;
            remaining = writer->next;
            writer->next = nullptr;

            // Update state: set kWriterLocked, keep kWriterWaiting if
            // there are more writers in remaining
            bool has_writer_waiting = false;
            for (auto* w = remaining; w; w = w->next) {
                if (!w->is_reader) { has_writer_waiting = true; break; }
            }

            auto new_state = kWriterLocked
                             | (has_writer_waiting ? kWriterWaiting : 0u);
            state_.store(new_state, std::memory_order_release);
        }
    }

    // Re-push remaining waiters (if any), merging with new arrivals
    if (remaining) {
        auto cur = waiters_.load(std::memory_order_acquire);
        do {
            // Find tail of remaining list
            auto* tail = remaining;
            while (tail->next) {
                tail = tail->next;
            }
            // Link: remaining -> ... -> tail -> existing
            tail->next = cur;
        } while (!waiters_.compare_exchange_weak(
            cur, remaining,
            std::memory_order_release,
            std::memory_order_acquire));
    }

    // Route woken waiters via executor
    auto* w = to_wake_list;
    while (w) {
        auto* next = w->next;
        w->next = nullptr;
        route_waiter(w);
        w = next;
    }
}

// ── route_waiter(): send a single waiter to its original worker thread ──

void AffinitySharedMutex::route_waiter(Waiter* waiter) {
    auto handle = waiter->handle;
    auto worker_id = waiter->worker_id;

    auto* ws_executor = WorkStealingExecutor::current_executor();

    if (ws_executor && worker_id != SIZE_MAX) {
        ws_executor->add_to_worker(worker_id, [handle]() mutable {
            handle.resume();
        });
    } else {
        handle.resume();
    }
}

}  // namespace quant::infra