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
        auto v = waiters_.load(std::memory_order_acquire);
        return reinterpret_cast<uintptr_t>(v) & kPostedBit;
    }

    bool try_wait() const noexcept {
        return ready();
    }

    // ── Reset (only safe when no waiters exist) ──

    void reset() noexcept {
        // Clear posted bit; only safe with no waiters
        auto* v = waiters_.load(std::memory_order_relaxed);
        waiters_.store(v, std::memory_order_release);
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

            // Single CAS: atomically checks posted bit AND enqueues.
            // No window between "check state" and "CAS into waiters".
            auto* old = baton.waiters_.load(std::memory_order_acquire);

            do {
                if (reinterpret_cast<uintptr_t>(old) & kPostedBit) {
                    // Already posted — don't suspend
                    handle.resume();
                    return;
                }
                node.next = clear_posted(old);
            } while (!baton.waiters_.compare_exchange_weak(
                old, &node,
                std::memory_order_release,
                std::memory_order_acquire));
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

    // Posted bit encoded in waiters_ pointer low bit (pointer is 4/8-byte aligned).
    // This merges state_ and waiters_ into a single atomic, eliminating the
    // "check state_ then CAS waiters_" race window in await_suspend.
    static constexpr uintptr_t kPostedBit = 1;
    static WaiterNode* clear_posted(WaiterNode* p) noexcept {
        return reinterpret_cast<WaiterNode*>(
            reinterpret_cast<uintptr_t>(p) & ~kPostedBit);
    }

    std::atomic<WaiterNode*> waiters_{nullptr};
};

}  // namespace quant::infra
