// affinity_shared_mutex.h -- Executor-routed SharedMutex with thread affinity
//
// Coroutine-friendly read-write lock with thread-affine wake-up.
// Replaces folly::coro::SharedMutex for our affinity-based scheduling model.
//
// Design: state word encodes reader count + writer flags; intrusive waiter
// stack for blocked coroutines. Wake-up routes through WorkStealingExecutor
// via add_to_worker(worker_id, handle) — same pattern as AffinityBaton/Mutex.
//
// Writer starvation prevention: kWriterWaiting flag blocks new readers from
// acquiring the lock while a writer is queued. Existing readers finish and
// release; the writer gets the lock next. This ensures writers always
// eventually acquire the lock under continuous reader load.
#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>

// Forward-declare folly::coro::Task for co_scoped_lock() return types.
namespace folly::coro { template<typename T> class Task; }

namespace quant::infra {

class WorkStealingExecutor;

// ── AffinitySharedMutex ──
//
// Coroutine-friendly read-write lock with thread-affine wake-up.
//
// API:
//   co_await mutex.co_lock()            -- acquire exclusive (write) lock
//   co_await mutex.co_shared_lock()     -- acquire shared (read) lock
//   mutex.co_scoped_lock()              -- acquire exclusive, return RAII guard
//   mutex.co_scoped_shared_lock()       -- acquire shared, return RAII guard
//   mutex.try_lock()                    -- non-blocking exclusive try
//   mutex.try_lock_shared()             -- non-blocking shared try
//   mutex.unlock()                      -- release exclusive lock
//   mutex.unlock_shared()               -- release shared lock
//
class AffinitySharedMutex {
public:
    AffinitySharedMutex() noexcept = default;

    ~AffinitySharedMutex() {
        // Resume any remaining waiters inline (shouldn't happen in correct code)
        auto* w = waiters_.exchange(nullptr, std::memory_order_acq_rel);
        while (w) {
            auto* next = w->next;
            w->handle.resume();
            w = next;
        }
    }

    AffinitySharedMutex(const AffinitySharedMutex&) = delete;
    AffinitySharedMutex& operator=(const AffinitySharedMutex&) = delete;

    // ── Intrusive waiter node ──

    struct Waiter {
        std::coroutine_handle<> handle;
        size_t worker_id;
        bool is_reader;     // true = shared lock waiter, false = exclusive
        Waiter* next;
    };

    // ── RAII guards ──

    class ExclusiveGuard {
    public:
        explicit ExclusiveGuard(AffinitySharedMutex& m) noexcept : mutex_(&m) {}
        ~ExclusiveGuard() { if (mutex_) mutex_->unlock(); }

        ExclusiveGuard(const ExclusiveGuard&) = delete;
        ExclusiveGuard& operator=(const ExclusiveGuard&) = delete;

        ExclusiveGuard(ExclusiveGuard&& o) noexcept : mutex_(o.mutex_) { o.mutex_ = nullptr; }
        ExclusiveGuard& operator=(ExclusiveGuard&& o) noexcept {
            if (this != &o) { if (mutex_) mutex_->unlock(); mutex_ = o.mutex_; o.mutex_ = nullptr; }
            return *this;
        }
    private:
        AffinitySharedMutex* mutex_;
    };

    class SharedGuard {
    public:
        explicit SharedGuard(AffinitySharedMutex& m) noexcept : mutex_(&m) {}
        ~SharedGuard() { if (mutex_) mutex_->unlock_shared(); }

        SharedGuard(const SharedGuard&) = delete;
        SharedGuard& operator=(const SharedGuard&) = delete;

        SharedGuard(SharedGuard&& o) noexcept : mutex_(o.mutex_) { o.mutex_ = nullptr; }
        SharedGuard& operator=(SharedGuard&& o) noexcept {
            if (this != &o) { if (mutex_) mutex_->unlock_shared(); mutex_ = o.mutex_; o.mutex_ = nullptr; }
            return *this;
        }
    private:
        AffinitySharedMutex* mutex_;
    };

    // ── Exclusive lock awaiter ──

    struct ExclusiveLockAwaiter {
        AffinitySharedMutex& mutex;
        Waiter node;

        bool await_ready() const noexcept {
            // Try to CAS state_ from 0 to kWriterLocked (fast uncontested path)
            uint32_t expected = 0;
            return mutex.state_.compare_exchange_strong(
                expected, kWriterLocked,
                std::memory_order_acquire,
                std::memory_order_relaxed);
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            node.handle = handle;
            node.worker_id = current_worker_id();
            node.is_reader = false;
            node.next = nullptr;

            auto* waiter_ptr = &node;

            // CAS loop: try to acquire or enqueue
            auto old_state = mutex.state_.load(std::memory_order_acquire);

            do {
                // If state is 0 (unlocked), try to acquire exclusive lock
                if (old_state == 0) {
                    if (mutex.state_.compare_exchange_strong(
                            old_state, kWriterLocked,
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        return false;  // Acquired, don't suspend
                    }
                    continue;  // CAS failed, retry with new old_state
                }

                // Lock is held. Set kWriterWaiting to block new readers,
                // then enqueue ourselves.
                auto new_state = old_state | kWriterWaiting;
                if (mutex.state_.compare_exchange_strong(
                        old_state, new_state,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    // Successfully set kWriterWaiting. Now enqueue.
                    break;
                }
                // CAS failed, retry
            } while (true);

            // kWriterWaiting is set. Push onto waiter stack.
            auto old_head = mutex.waiters_.load(std::memory_order_acquire);
            do {
                waiter_ptr->next = old_head;
            } while (!mutex.waiters_.compare_exchange_weak(
                old_head, waiter_ptr,
                std::memory_order_release,
                std::memory_order_acquire));

            return true;  // Suspended
        }

        void await_resume() const noexcept {}
    };

    // ── Shared lock awaiter ──

    struct SharedLockAwaiter {
        AffinitySharedMutex& mutex;
        Waiter node;

        bool await_ready() const noexcept {
            // Fast uncontested path: try to increment reader count atomically
            auto s = mutex.state_.load(std::memory_order_acquire);
            if (s & (kWriterLocked | kWriterWaiting)) return false;
            return mutex.state_.compare_exchange_strong(
                s, s + kReaderOne,
                std::memory_order_acquire,
                std::memory_order_relaxed);
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            node.handle = handle;
            node.worker_id = current_worker_id();
            node.is_reader = true;
            node.next = nullptr;

            auto* waiter_ptr = &node;

            // CAS loop: try to acquire shared or enqueue
            auto old_state = mutex.state_.load(std::memory_order_acquire);

            do {
                // If no writer locked/waiting, try to increment reader count
                if ((old_state & (kWriterLocked | kWriterWaiting)) == 0) {
                    auto new_state = old_state + kReaderOne;
                    if (mutex.state_.compare_exchange_strong(
                            old_state, new_state,
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        return false;  // Acquired shared, don't suspend
                    }
                    continue;  // CAS failed, retry
                }

                // Writer is active or waiting. Must enqueue.
                // If writer is waiting, set kWriterWaiting (may already be set).
                auto new_state = old_state | kWriterWaiting;
                if (mutex.state_.compare_exchange_strong(
                        old_state, new_state,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    break;  // State updated, enqueue now
                }
                // CAS failed, retry
            } while (true);

            // Push onto waiter stack.
            auto old_head = mutex.waiters_.load(std::memory_order_acquire);
            do {
                waiter_ptr->next = old_head;
            } while (!mutex.waiters_.compare_exchange_weak(
                old_head, waiter_ptr,
                std::memory_order_release,
                std::memory_order_acquire));

            return true;  // Suspended
        }

        void await_resume() const noexcept {}
    };

    // ── co_lock(): acquire exclusive lock ──

    ExclusiveLockAwaiter co_lock() noexcept {
        return ExclusiveLockAwaiter{*this, Waiter{}};
    }

    // ── co_shared_lock(): acquire shared lock ──

    SharedLockAwaiter co_shared_lock() noexcept {
        return SharedLockAwaiter{*this, Waiter{}};
    }

    // ── co_scoped_lock() / co_scoped_shared_lock() ──
    // Defined in .cc (needs folly::coro::Task full definition)

    folly::coro::Task<ExclusiveGuard> co_scoped_lock();
    folly::coro::Task<SharedGuard> co_scoped_shared_lock();

    // ── try_lock(): non-blocking exclusive try ──

    bool try_lock() noexcept {
        uint32_t expected = 0;
        return state_.compare_exchange_strong(
            expected, kWriterLocked,
            std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    // ── try_lock_shared(): non-blocking shared try ──

    bool try_lock_shared() noexcept {
        auto old_state = state_.load(std::memory_order_acquire);
        do {
            if (old_state & (kWriterLocked | kWriterWaiting)) {
                return false;  // Writer active or waiting
            }
            auto new_state = old_state + kReaderOne;
            if (state_.compare_exchange_weak(
                    old_state, new_state,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
        } while (true);
    }

    // ── unlock(): release exclusive lock ──

    void unlock();

    // ── unlock_shared(): release shared lock ──

    void unlock_shared();

private:
    static size_t current_worker_id();

    // Helper: wake waiters after releasing lock
    void wake_waiters();

    // Helper: route a single waiter via executor
    static void route_waiter(Waiter* waiter);

    // ── State encoding ──
    //   bit 0: kWriterLocked   — exclusive lock held
    //   bit 1: kWriterWaiting  — writer queued, blocks new readers
    //   bits 2+: reader count  — number of active shared holders
    static constexpr uint32_t kWriterLocked   = 1u << 0;
    static constexpr uint32_t kWriterWaiting  = 1u << 1;
    static constexpr uint32_t kReaderShift    = 2;
    static constexpr uint32_t kReaderOne      = 1u << kReaderShift;

    static uint32_t reader_count(uint32_t state) noexcept {
        return state >> kReaderShift;
    }

    std::atomic<uint32_t> state_{0};
    std::atomic<Waiter*> waiters_{nullptr};
};

}  // namespace quant::infra