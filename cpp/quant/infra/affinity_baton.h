// affinity_baton.h -- Executor-routed Baton with thread affinity
//
// Complete replacement for folly::coro::Baton that routes post() through
// the WorkStealingExecutor to guarantee thread affinity: each waiter
// is resumed on the worker thread where it originally co_await'ed.
//
// Design: intrusive linked list of WaiterNode, each recording the
// waiter's worker_id. post() atomically drains the list and calls
// executor.add_to_worker(id, handle) for each waiter.
#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>

namespace quant::infra {

class WorkStealingExecutor;

namespace detail {
    extern size_t (*get_current_worker_id)();
}

// ── AffinityBaton ──
//
// Multi-waiter Baton with executor-based routing.
//
// API parity with folly::coro::Baton:
//   co_await baton          -- suspend until posted
//   baton.ready()           -- query posted state
//   baton.try_wait()        -- non-blocking check
//   baton.reset()           -- reset to not-ready
//   baton.post(executor)    -- resume waiters via executor (affinity!)
//   baton.post_direct()     -- resume waiters inline (no executor)
//
class AffinityBaton {
public:
    AffinityBaton() noexcept = default;

    ~AffinityBaton() {
        // If there are still waiters (baton was destroyed before post),
        // resume them inline so they can continue and eventually notice.
        auto* waiters = waiters_.exchange(nullptr, std::memory_order_acq_rel);
        if (waiters) {
            resume_chain(waiters, nullptr);
        }
    }

    AffinityBaton(const AffinityBaton&) = delete;
    AffinityBaton& operator=(const AffinityBaton&) = delete;

    // ── Query ──

    bool ready() const noexcept {
        return state_.load(std::memory_order_acquire) == State::POSTED;
    }

    bool try_wait() const noexcept {
        return ready();
    }

    // ── Reset (only safe when no waiters exist) ──

    void reset() noexcept {
        state_.store(State::NOT_READY, std::memory_order_release);
    }

    // ── Intrusive waiter node ──

    struct WaiterNode {
        std::coroutine_handle<> handle;
        size_t worker_id;     // waiter's worker_id (SIZE_MAX = external thread)
        WaiterNode* next;
    };

    // ── Awaiter (returned by operator co_await) ──

    struct Awaiter {
        AffinityBaton& baton;
        WaiterNode node;

        bool await_ready() const noexcept {
            return baton.ready();
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            node.handle = handle;
            node.worker_id = current_worker_id();
            node.next = nullptr;

            // CAS loop to add this node to the waiters list
            auto* old_head = baton.waiters_.load(std::memory_order_acquire);

            do {
                // Double-check: if baton became posted while we were
                // preparing, resume immediately instead of suspending.
                if (baton.state_.load(std::memory_order_acquire) ==
                    State::POSTED) {
                    handle.resume();
                    return;
                }
                node.next = old_head;
            } while (!baton.waiters_.compare_exchange_weak(
                old_head, &node,
                std::memory_order_release,
                std::memory_order_acquire));

            // Successfully added to waiters list and suspended.
        }

        void await_resume() const noexcept {}
    };

    Awaiter operator co_await() noexcept {
        return Awaiter{*this, WaiterNode{}};
    }

    // ── Post with executor routing ──
    //
    // Atomically drains the waiter chain and routes each waiter's
    // continuation through executor.add_to_worker(worker_id, handle).
    // This guarantees each waiter resumes on its original worker thread.
    void post(WorkStealingExecutor& executor);

    // ── Direct post (no executor) ──
    //
    // Resume all waiters inline on the calling thread.
    // Use for: destruction cleanup, test code, non-executor contexts.
    void post_direct() noexcept;

private:
    static size_t current_worker_id();

    static void resume_chain(WaiterNode* waiters,
                             WorkStealingExecutor* executor);

    enum class State : uint8_t {
        NOT_READY,
        POSTED,
    };

    std::atomic<State> state_{State::NOT_READY};
    std::atomic<WaiterNode*> waiters_{nullptr};
};

}  // namespace quant::infra
