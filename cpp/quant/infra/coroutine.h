// coroutine.h — Lightweight coroutine primitives for quant system
//
// Provides CoTask<T>, Baton, CoroutineMutex and type aliases that
// can be swapped for folly::coro equivalents when QUANT_USE_FOLLY
// is enabled.
#pragma once

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace quant::infra {

// ── Lightweight coroutine Task (named CoTask to avoid clash with
//    the type-erased Task class in thread_pool.h) ──
// Eagerly-started coroutine with continuation chaining.
template<typename T = void>
class CoTask {
public:
    // promise_type defined below after CoTask is complete
    struct promise_type;

    // ── Awaiter for co_await CoTask ──
    struct Awaiter {
        std::coroutine_handle<promise_type> handle;

        bool await_ready() noexcept { return !handle || handle.done(); }

        void await_suspend(std::coroutine_handle<> caller) noexcept {
            handle.promise().continuation_ = caller;
        }

        auto await_resume() {
            if (handle.promise().exception_) {
                std::rethrow_exception(handle.promise().exception_);
            }
            if constexpr (!std::is_void_v<T>) {
                return std::move(*handle.promise().result_);
            }
        }
    };

    CoTask() noexcept = default;
    explicit CoTask(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

    CoTask(CoTask&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    CoTask& operator=(CoTask&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    CoTask(const CoTask&) = delete;
    CoTask& operator=(const CoTask&) = delete;

    ~CoTask() { destroy(); }

    Awaiter operator co_await() && noexcept { return Awaiter{handle_}; }

    bool valid() const noexcept { return handle_ != nullptr; }

private:
    void destroy() {
        if (handle_) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    std::coroutine_handle<promise_type> handle_ = nullptr;
};

// ── Non-void promise_type ──
template<typename T>
struct CoTask<T>::promise_type {
    auto get_return_object() {
        return CoTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_never initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) noexcept {
                auto& p = std::coroutine_handle<promise_type>::from_address(h.address()).promise();
                if (p.continuation_) {
                    p.continuation_.resume();
                }
            }
            void await_resume() noexcept {}
        };
        return FinalAwaiter{};
    }

    template<typename U>
    void return_value(U&& value) {
        result_.emplace(std::forward<U>(value));
    }

    void unhandled_exception() { exception_ = std::current_exception(); }

    std::optional<T> result_;
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_;
};

// ── Void promise_type ──
template<>
struct CoTask<void>::promise_type {
    auto get_return_object() {
        return CoTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_never initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) noexcept {
                auto& p = std::coroutine_handle<promise_type>::from_address(h.address()).promise();
                if (p.continuation_) {
                    p.continuation_.resume();
                }
            }
            void await_resume() noexcept {}
        };
        return FinalAwaiter{};
    }

    void return_void() noexcept {}

    void unhandled_exception() { exception_ = std::current_exception(); }

    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_;
};

// ── Baton: coroutine-friendly synchronization ──
class Baton {
public:
    Baton() = default;

    void post() {
        std::lock_guard lock(mutex_);
        posted_ = true;
        cv_.notify_all();
    }

    bool try_wait() const {
        return posted_.load(std::memory_order_acquire);
    }

    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return posted_.load(); });
    }

    struct Awaiter {
        Baton& baton;

        bool await_ready() noexcept {
            return baton.posted_.load(std::memory_order_acquire);
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            std::unique_lock lock(baton.mutex_);
            baton.cv_.wait(lock, [&] { return baton.posted_.load(); });
            handle.resume();
        }

        void await_resume() noexcept {}
    };

    Awaiter operator co_await() noexcept { return Awaiter{*this}; }

    void reset() {
        std::lock_guard lock(mutex_);
        posted_ = false;
    }

private:
    std::atomic<bool> posted_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

// ── Coroutine-aware Mutex ──
class CoroutineMutex {
public:
    CoroutineMutex() = default;

    struct LockAwaiter {
        CoroutineMutex& mtx;
        bool acquired;

        bool await_ready() noexcept { return false; }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            bool expected = false;
            if (mtx.locked_.compare_exchange_strong(expected, true,
                    std::memory_order_acquire)) {
                acquired = true;
                handle.resume();
            } else {
                std::lock_guard lock(mtx.mutex_);
                mtx.waiters_.push_back(handle);
            }
        }

        void await_resume() noexcept {}
    };

    LockAwaiter operator co_await() noexcept { return LockAwaiter{*this, false}; }

    void unlock() {
        locked_.store(false, std::memory_order_release);
        std::lock_guard lock(mutex_);
        if (!waiters_.empty()) {
            auto next = waiters_.front();
            waiters_.erase(waiters_.begin());
            locked_.store(true, std::memory_order_release);
            next.resume();
        }
    }

private:
    std::atomic<bool> locked_{false};
    std::mutex mutex_;
    std::vector<std::coroutine_handle<>> waiters_;
};

}  // namespace quant::infra
