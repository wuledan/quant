// affinity_mutex.h -- Executor-routed Mutex with thread affinity
//
// Complete replacement for folly::coro::Mutex that routes unlock()
// through the WorkStealingExecutor to guarantee thread affinity: each
// waiter is resumed on the worker thread where it originally co_await'ed.
//
// Design: single atomic state word encoding lock flag + waiter list.
//   state_ == 0           : unlocked, no waiters
//   state_ == kLockedFlag : locked, no waiters
//   state_ & ~kLockedFlag : locked, with waiters (pointer to Waiter stack)
//
// This avoids all races between lock release and waiter enqueue because
// the state transition is atomic.
#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>

// Forward-declare folly::coro::Task for co_scoped_lock() return type.
// Full definition is in <folly/coro/Task.h>, included via coroutine.h in the .cc.
namespace folly::coro { template<typename T> class Task; }

namespace quant::infra {

class WorkStealingExecutor;

// ── AffinityMutex ──
//
// Coroutine-friendly mutex with thread-affine wake-up.
//
// API parity with folly::coro::Mutex:
//   co_await mutex.co_lock()   -- acquire exclusive lock (coroutine)
//   mutex.co_scoped_lock()     -- acquire and return RAII guard
//   mutex.try_lock()           -- non-blocking try
//   mutex.unlock()             -- release (called by guard)
//
class AffinityMutex {
public:
    AffinityMutex() noexcept = default;

    ~AffinityMutex() {
        // If there are still waiters (mutex destroyed while locked with
        // waiters), resume them inline so they can continue and eventually
        // notice. This should not happen in correct code.
        auto state = state_.load(std::memory_order_acquire);
        if (auto* waiters = extract_waiters(state)) {
            while (waiters) {
                auto* next = waiters->next;
                waiters->handle.resume();
                waiters = next;
            }
        }
    }

    AffinityMutex(const AffinityMutex&) = delete;
    AffinityMutex& operator=(const AffinityMutex&) = delete;

    // ── Intrusive waiter node ──

    struct Waiter {
        std::coroutine_handle<> handle;
        size_t worker_id;  // waiter's worker_id (SIZE_MAX = external thread)
        Waiter* next;
    };

    // ── Awaiter for co_lock() ──

    struct LockAwaiter {
        AffinityMutex& mutex;
        Waiter node;

        bool await_ready() const noexcept {
            // Try to acquire without suspending: CAS state_ from 0 to kLockedFlag
            uintptr_t expected = 0;
            return mutex.state_.compare_exchange_strong(
                expected, kLockedFlag,
                std::memory_order_acquire,
                std::memory_order_relaxed);
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            node.handle = handle;
            node.worker_id = current_worker_id();
            node.next = nullptr;

            // We failed to acquire the lock (await_ready returned false).
            // Enqueue ourselves onto the waiter stack.
            auto* waiter_ptr = &node;

            // CAS loop to push onto the waiter stack.
            auto old_state = mutex.state_.load(std::memory_order_acquire);

            do {
                // Double-check: if lock became free, try to acquire directly
                if (old_state == 0) {
                    if (mutex.state_.compare_exchange_strong(
                            old_state, kLockedFlag,
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        // Got the lock, don't suspend
                        return false;
                    }
                    // CAS failed, someone else grabbed it. old_state is updated.
                    continue;
                }

                // Lock is held. Push our waiter onto the stack.
                // Existing waiters list is at (old_state & ~kLockedFlag).
                auto* old_waiters = reinterpret_cast<Waiter*>(
                    old_state & ~kLockedFlag);
                waiter_ptr->next = old_waiters;

                uintptr_t new_state = reinterpret_cast<uintptr_t>(waiter_ptr)
                                      | kLockedFlag;

                if (mutex.state_.compare_exchange_weak(
                        old_state, new_state,
                        std::memory_order_release,
                        std::memory_order_acquire)) {
                    // Successfully enqueued; suspend.
                    return true;
                }
                // CAS failed, state changed. old_state is updated. Retry.
                // Note: waiter_ptr->next may be stale now, but it will be
                // re-set on the next iteration.
            } while (true);
        }

        void await_resume() const noexcept {
            // We now hold the lock
        }
    };

    // ── co_lock(): acquire exclusive lock ──

    LockAwaiter co_lock() noexcept {
        return LockAwaiter{*this, Waiter{}};
    }

    // ── RAII lock guard ──

    class AffinityMutexLock {
    public:
        explicit AffinityMutexLock(AffinityMutex& mutex) noexcept
            : mutex_(&mutex) {}

        ~AffinityMutexLock() {
            if (mutex_) {
                mutex_->unlock();
            }
        }

        AffinityMutexLock(const AffinityMutexLock&) = delete;
        AffinityMutexLock& operator=(const AffinityMutexLock&) = delete;

        AffinityMutexLock(AffinityMutexLock&& other) noexcept
            : mutex_(other.mutex_) {
            other.mutex_ = nullptr;
        }

        AffinityMutexLock& operator=(AffinityMutexLock&& other) noexcept {
            if (this != &other) {
                if (mutex_) {
                    mutex_->unlock();
                }
                mutex_ = other.mutex_;
                other.mutex_ = nullptr;
            }
            return *this;
        }

    private:
        AffinityMutex* mutex_;
    };

    // ── co_scoped_lock(): acquire and return RAII guard ──
    // Defined in affinity_mutex.cc (needs folly::coro::Task full definition)
    folly::coro::Task<AffinityMutexLock> co_scoped_lock();

    // ── try_lock(): non-blocking try ──

    bool try_lock() noexcept {
        uintptr_t expected = 0;
        return state_.compare_exchange_strong(
            expected, kLockedFlag,
            std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    // ── unlock(): release lock, wake next waiter if any ──

    void unlock();

private:
    static size_t current_worker_id();

    static constexpr uintptr_t kLockedFlag = 1;

    static Waiter* extract_waiters(uintptr_t state) noexcept {
        return reinterpret_cast<Waiter*>(state & ~kLockedFlag);
    }

    // Combined state: either 0 (unlocked), kLockedFlag (locked, no waiters),
    // or (waiter_ptr | kLockedFlag) (locked, with waiters).
    // Waiter pointers are always aligned (at least 2-byte aligned for
    // struct with pointer + size_t members), so bit 0 is safe for the flag.
    std::atomic<uintptr_t> state_{0};
};

}  // namespace quant::infra
