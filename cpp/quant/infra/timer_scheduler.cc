// timer_scheduler.cc -- TimerScheduler implementation
//
// The timer thread owns a dedicated folly::EventBase + folly::HHWheelTimer.
// All timer registrations/cancellations post to the EventBase thread.
// Coroutine continuations (co_sleep) are routed back to the originating
// worker via WorkStealingExecutor::add_to_worker().

#include "cpp/quant/infra/timer_scheduler.h"
#include "cpp/quant/infra/work_stealing_executor.h"
#include "cpp/quant/infra/thread_local_stats.h"

#include <folly/futures/Future.h>
#include <folly/Unit.h>

#include <future>
#include <utility>

namespace quant::infra {

// ── Construction / destruction ──

TimerScheduler::TimerScheduler(WorkStealingExecutor& executor)
    : executor_(executor) {}

TimerScheduler::~TimerScheduler() {
    stop();
}

// ── Lifecycle ──

void TimerScheduler::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;  // already running
    }

    // Synchronisation: the calling thread waits until the timer thread
    // has initialised EventBase / HHWheelTimer.
    std::promise<void> ready;
    auto ready_future = ready.get_future();

    timer_thread_ = std::thread([this, ready = std::move(ready)]() mutable {
        folly::EventBase eb;
        // HHWheelTimer with 10 ms tick, 20 ticks/wheel, 3 wheels = 2 s max
        // newTimer returns std::unique_ptr with custom deleter.
        auto wt = folly::HHWheelTimer::newTimer(
            &eb,
            std::chrono::milliseconds(10));

        event_base_ = &eb;
        wheel_timer_ = wt.get();

        ready.set_value();

        eb.loopForever();

        // wt destructor runs here, cleaning up the wheel timer.
        event_base_ = nullptr;
        wheel_timer_ = nullptr;
    });

    ready_future.wait();
}

void TimerScheduler::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;  // wasn't running
    }

    // Signal the EventBase to stop looping.
    if (event_base_) {
        event_base_->terminateLoopSoon();
    }

    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }

    // At this point the timer thread has exited and event_base_ / wheel_timer_
    // are no longer valid.  Clean up any remaining callback objects.
    active_timers_.clear();
}

// ── co_sleep ──

folly::coro::Task<void> TimerScheduler::co_sleep(
    std::chrono::nanoseconds duration) {
    // Record the caller's worker id for thread-affine resume.
    size_t caller_worker_id = WorkStealingExecutor::current_worker_id();

    auto [promise, future] =
        folly::makePromiseContract<folly::Unit>();

    auto shared_promise =
        std::make_shared<folly::Promise<folly::Unit>>(std::move(promise));

    // Post the timer registration to the EventBase thread.
    event_base_->runInEventBaseThread(
        [this, shared_promise, caller_worker_id, duration]() {
            struct SleepCallback : folly::HHWheelTimer::Callback {
                std::shared_ptr<folly::Promise<folly::Unit>> promise;
                WorkStealingExecutor* executor;
                size_t caller_worker_id;

                SleepCallback(
                    std::shared_ptr<folly::Promise<folly::Unit>> p,
                    WorkStealingExecutor* e,
                    size_t id)
                    : promise(std::move(p)), executor(e), caller_worker_id(id) {}

                void timeoutExpired() noexcept override {
                    // Route the promise fulfilment (and thus coroutine
                    // resumption) back to the caller's worker thread.
                    executor->add_to_worker(
                        caller_worker_id,
                        [p = std::move(*promise)]() mutable {
                            p.setValue(folly::Unit{});
                        });
                    delete this;
                }

                void callbackCanceled() noexcept override {
                    delete this;
                }
            };

            StatRegistry::instance().increment("timer.scheduled");

            auto* cb = new SleepCallback(shared_promise, &executor_, caller_worker_id);
            wheel_timer_->scheduleTimeout(cb, std::chrono::duration_cast<std::chrono::milliseconds>(duration));
        });

    // Suspend the coroutine until the future is fulfilled.
    co_await std::move(future);
}

// ── schedule_at ──

TimerScheduler::TimerId TimerScheduler::schedule_at(
    std::chrono::steady_clock::time_point deadline,
    size_t target_worker_id,
    folly::Function<void()> callback) {
    auto id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);

    auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - std::chrono::steady_clock::now());
    if (delay.count() < 0) {
        delay = std::chrono::nanoseconds(0);
    }

    auto cb = std::make_unique<TimerCallback>();
    cb->id = id;
    cb->executor = &executor_;
    cb->target_worker_id = target_worker_id;
    cb->callback =
        std::make_shared<folly::Function<void()>>(std::move(callback));
    cb->scheduler = this;
    cb->is_periodic = false;
    cb->period = std::chrono::nanoseconds(0);

    post_schedule_timer(std::move(cb), delay);
    return id;
}

// ── schedule_periodic ──

TimerScheduler::TimerId TimerScheduler::schedule_periodic(
    std::chrono::nanoseconds interval,
    size_t target_worker_id,
    folly::Function<void()> callback) {
    auto id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);

    auto cb = std::make_unique<TimerCallback>();
    cb->id = id;
    cb->executor = &executor_;
    cb->target_worker_id = target_worker_id;
    cb->callback =
        std::make_shared<folly::Function<void()>>(std::move(callback));
    cb->scheduler = this;
    cb->is_periodic = true;
    cb->period = interval;

    post_schedule_timer(std::move(cb), interval);
    return id;
}

// ── cancel ──

void TimerScheduler::cancel(TimerId id) {
    event_base_->runInEventBaseThread([this, id]() {
        auto it = active_timers_.find(id);
        if (it != active_timers_.end()) {
            it->second->cancelTimeout();
            // callbackCanceled is a no-op; the unique_ptr deletion
            // handles cleanup of the callback object.
            active_timers_.erase(it);
            StatRegistry::instance().increment("timer.cancelled");
        }
    });
}

// ── Internal helpers ──

void TimerScheduler::post_schedule_timer(
    std::unique_ptr<TimerCallback> cb,
    std::chrono::nanoseconds delay) {
    auto* raw = cb.release();

    event_base_->runInEventBaseThread([this, raw, delay]() {
        auto* cb = raw;
        auto id = cb->id;

        active_timers_.emplace(id, std::unique_ptr<TimerCallback>(cb));
        StatRegistry::instance().increment("timer.scheduled");

        wheel_timer_->scheduleTimeout(cb, std::chrono::duration_cast<std::chrono::milliseconds>(delay));
    });
}

// ── TimerCallback::timeoutExpired ──

void TimerScheduler::TimerCallback::timeoutExpired() noexcept {
    StatRegistry::instance().increment("timer.fired");

    if (is_periodic) {
        // Reschedule the next period first.
        scheduler->wheel_timer_->scheduleTimeout(this, std::chrono::duration_cast<std::chrono::milliseconds>(period));

        // Copy the shared_ptr and invoke the function on the target worker.
        auto cb = callback;
        executor->add_to_worker(
            target_worker_id,
            [cb]() { (*cb)(); });
    } else {
        // One-shot: move the function out, then erase from the map
        // (which deletes `this`).
        auto cb = std::move(*callback);
        scheduler->active_timers_.erase(id);
        executor->add_to_worker(
            target_worker_id, std::move(cb));
    }
}

// ── TimerCallback::callbackCanceled ──

void TimerScheduler::TimerCallback::callbackCanceled() noexcept {
    // Nothing to do — the owner (cancel() or ~TimerScheduler)
    // handles cleanup via the active_timers_ map.
}

}  // namespace quant::infra
