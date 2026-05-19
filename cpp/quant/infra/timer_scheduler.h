// timer_scheduler.h — Coroutine-friendly timer with thread affinity
//
// Provides:
//   - co_sleep(duration): coroutine suspension with automatic resume
//     on the calling worker thread
//   - schedule_at/deadline: one-shot timer callbacks with thread affinity
//   - schedule_periodic: recurring timer callbacks
//   - cancel: cancel a pending timer
//
// Implementation: dedicated timer thread runs folly::EventBase +
// folly::HHWheelTimer. All timer registrations/cancellations route
// through the EventBase to remain thread-safe.
#pragma once

#include <folly/Function.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/HHWheelTimer.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>

namespace quant::infra {

class WorkStealingExecutor;

class TimerScheduler {
public:
    using TimerId = uint64_t;

    explicit TimerScheduler(WorkStealingExecutor& executor);
    ~TimerScheduler();

    TimerScheduler(const TimerScheduler&) = delete;
    TimerScheduler& operator=(const TimerScheduler&) = delete;

    // ── Lifecycle ──
    void start();
    void stop();

    // ── Coroutine-friendly sleep ──
    //
    // Suspends the current coroutine for the given duration. When the
    // timer expires the coroutine resumes on the worker thread where
    // co_sleep was called (or the global queue for external threads).
    folly::coro::Task<void> co_sleep(std::chrono::nanoseconds duration);

    // ── One-shot timer callback ──
    //
    // Calls callback on the worker identified by target_worker_id when
    // deadline is reached. Returns a TimerId for cancellation.
    TimerId schedule_at(
        std::chrono::steady_clock::time_point deadline,
        size_t target_worker_id,
        folly::Function<void()> callback);

    // ── Periodic timer callback ──
    //
    // Calls callback on target_worker_id every interval. Returns a
    // TimerId for cancellation. The callback is copied each period.
    TimerId schedule_periodic(
        std::chrono::nanoseconds interval,
        size_t target_worker_id,
        folly::Function<void()> callback);

    // ── Cancel ──
    //
    // Cancels a previously scheduled timer. Safe to call from any
    // thread; the cancellation is routed through the EventBase.
    void cancel(TimerId id);

private:
    // Internal callback wrapper for HHWheelTimer.
    struct TimerCallback : folly::HHWheelTimer::Callback {
        TimerId id;
        WorkStealingExecutor* executor;
        size_t target_worker_id;
        std::shared_ptr<folly::Function<void()>> callback;
        TimerScheduler* scheduler;
        bool is_periodic{false};
        std::chrono::nanoseconds period{0};

        void timeoutExpired() noexcept override;
        void callbackCanceled() noexcept override;
    };

    // Post a timer callback to the EventBase thread for scheduling.
    void post_schedule_timer(
        std::unique_ptr<TimerCallback> cb,
        std::chrono::nanoseconds delay);

    WorkStealingExecutor& executor_;

    std::atomic<bool> running_{false};

    // Owned by the timer thread; valid only while running_.
    folly::EventBase* event_base_{nullptr};
    folly::HHWheelTimer* wheel_timer_{nullptr};
    std::thread timer_thread_;

    std::atomic<TimerId> next_timer_id_{0};

    // Accessed only from the timer (EventBase) thread.
    std::unordered_map<TimerId, std::unique_ptr<TimerCallback>> active_timers_;
};

} // namespace quant::infra
