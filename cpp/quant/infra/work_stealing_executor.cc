// work_stealing_executor.cc -- Work-stealing executor implementation
//
// Each worker runs worker_loop() which does:
//   1. pop own Chase-Lev deque (LIFO, O(1), no contention)
//   2. read from global MPMC queue (external submissions)
//   3. steal from random victim's deque (FIFO, lock-free)
//   4. three-level backoff: PAUSE spin(10us) -> yield(3 rounds) -> futex park

#include "work_stealing_executor.h"
#include "affinity_baton.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>

namespace quant::infra {

// ── Thread-local worker identity ──

namespace {
    thread_local size_t tl_worker_id = WorkStealingExecutor::kExternalThread;
    thread_local WorkStealingExecutor* tl_executor = nullptr;

    // Simple xorshift64 PRNG per thread (no std::mutex needed)
    thread_local uint64_t tl_rng_state = 123456789 + reinterpret_cast<uintptr_t>(&tl_rng_state);

    uint64_t fast_rand() {
        uint64_t x = tl_rng_state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        tl_rng_state = x;
        return x;
    }
}

// ── Static worker ID queries ──

size_t WorkStealingExecutor::current_worker_id() {
    return tl_worker_id;
}

bool WorkStealingExecutor::is_pool_worker() {
    return tl_worker_id != kExternalThread;
}

// ── Construction / destruction ──

WorkStealingExecutor::WorkStealingExecutor(
    size_t num_workers, std::string name_prefix)
    : num_workers_(num_workers)
    , name_prefix_(std::move(name_prefix)) {
    workers_.reserve(num_workers_);
    for (size_t i = 0; i < num_workers_; ++i) {
        auto ws = std::make_unique<WorkerState>();
        ws->local_deque =
            std::make_unique<ChaseLevDeque<WorkItem>>(kLocalDequeCapacity);
        ws->worker_id = i;
        workers_.push_back(std::move(ws));
    }
}

WorkStealingExecutor::~WorkStealingExecutor() {
    force_stop();
}

// ── Lifecycle ──

void WorkStealingExecutor::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;  // already running
    }
    stopping_.store(false, std::memory_order_release);

    // Set the function pointer that AffinityBaton uses to resolve
    // the current worker ID. This enables thread affinity for
    // baton-based coroutine synchronization.
    detail::get_current_worker_id = []() -> size_t {
        return tl_worker_id;
    };

    for (auto& ws : workers_) {
        ws->thread = std::thread(
            [this, id = ws->worker_id]() { worker_loop(id); });
        // Set thread name
        auto thread_name = name_prefix_ + "-" + std::to_string(ws->worker_id);
        pthread_setname_np(ws->thread.native_handle(),
                           thread_name.substr(0, 15).c_str());
    }
}

void WorkStealingExecutor::stop() {
    stopping_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    // Wake all parked workers so they can exit
    for (auto& ws : workers_) {
        std::lock_guard lock(ws->park_mutex);
        ws->park_cv.notify_one();
    }

    for (auto& ws : workers_) {
        if (ws->thread.joinable()) {
            ws->thread.join();
        }
    }
}

void WorkStealingExecutor::force_stop() {
    stopping_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    for (auto& ws : workers_) {
        std::lock_guard lock(ws->park_mutex);
        ws->park_cv.notify_one();
    }

    for (auto& ws : workers_) {
        if (ws->thread.joinable()) {
            ws->thread.join();
        }
    }
}

// ── add() — Folly coroutine resume ──

void WorkStealingExecutor::add(folly::Func func) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    WorkItem item;
    item.func = std::move(func);
    item.is_coroutine_resume = true;

    if (tl_executor == this) {
        // Called from a pool worker -> thread affinity: push to local deque
        item.target_worker_id = tl_worker_id;
        workers_[tl_worker_id]->local_deque->push(std::move(item));
    } else {
        // Called from external thread -> push to global queue
        item.target_worker_id = kExternalThread;
        global_queue_.enqueue(std::move(item));
        wake_one_worker();
    }
}

// ── add_to_worker() — Thread-affine task submission ──

void WorkStealingExecutor::add_to_worker(size_t worker_id, folly::Func func) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    if (worker_id == kExternalThread) {
        // Caller was an external thread — route through global queue
        add(std::move(func));
        return;
    }

    WorkItem item;
    item.func = std::move(func);
    item.target_worker_id = worker_id;
    item.is_coroutine_resume = true;

    workers_[worker_id]->local_deque->push(std::move(item));
    wake_worker(worker_id);
}

// ── Worker loop ──

void WorkStealingExecutor::worker_loop(size_t my_id) {
    tl_worker_id = my_id;
    tl_executor = this;

    auto& me = *workers_[my_id];

    while (running_.load(std::memory_order_acquire)) {
        WorkItem item;
        bool found = false;

        // ── Step 1: pop own local deque (LIFO, O(1), lock-free) ──
        auto popped = me.local_deque->pop();
        if (popped) {
            item = std::move(*popped);
            found = true;
            me.local_pops.fetch_add(1, std::memory_order_relaxed);
        }

        // ── Step 2: read from global queue ──
        if (!found) {
            WorkItem global_item;
            if (global_queue_.try_dequeue(global_item)) {
                // If this task has affinity to a different worker, redirect it
                if (global_item.target_worker_id != kExternalThread &&
                    global_item.target_worker_id != my_id) {
                    workers_[global_item.target_worker_id]->local_deque->push(
                        std::move(global_item));
                    found = false;
                    // Continue to steal loop (don't execute in-flight)
                } else {
                    item = std::move(global_item);
                    found = true;
                }
            }
        }

        // ── Step 3: steal from random victim (FIFO, lock-free) ──
        if (!found) {
            for (size_t attempt = 0; attempt < kStealAttempts; ++attempt) {
                size_t victim = fast_rand() % num_workers_;
                if (victim == my_id) continue;

                auto stolen = workers_[victim]->local_deque->steal();
                if (stolen) {
                    workers_[victim]->steals_from_me.fetch_add(
                        1, std::memory_order_relaxed);
                    // If this task has thread affinity to a different
                    // worker, redirect it instead of executing locally.
                    if (stolen->target_worker_id != kExternalThread &&
                        stolen->target_worker_id != my_id) {
                        workers_[stolen->target_worker_id]->local_deque->
                            push(std::move(*stolen));
                        found = false;
                        break;
                    }
                    item = std::move(*stolen);
                    found = true;
                    break;
                }
            }
        }

        // ── Step 4: execute or three-level backoff ──
        if (found) {
            // Reset backoff counters
            me.spin_count = 0;
            me.yield_count = 0;

            // Execute
            auto task_start = std::chrono::steady_clock::now();
            item.execute();
            auto task_end = std::chrono::steady_clock::now();

            auto busy_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                task_end - task_start).count();
            me.busy_cycles_ns.fetch_add(
                static_cast<uint64_t>(busy_ns), std::memory_order_relaxed);
            me.tasks_completed.fetch_add(1, std::memory_order_relaxed);
        } else {
            // ── Three-level backoff: slow degrade, fast recover ──

            if (me.spin_count < kSpinMaxUs) {
                // Level 1: PAUSE spin (~100ns per iteration, low power)
#if defined(__x86_64__) || defined(__i386__)
                asm volatile("pause");
#elif defined(__aarch64__)
                asm volatile("yield");
#endif
                me.spin_count++;
            } else if (me.yield_count < kYieldRounds) {
                // Level 2: yield CPU time slice (~1-10us)
                std::this_thread::yield();
                me.yield_count++;
            } else {
                // Level 3: futex park (CPU utilization ~0%)
                me.parked.store(true, std::memory_order_release);
                {
                    std::unique_lock lock(me.park_mutex);
                    me.park_cv.wait(lock, [&]() {
                        return !me.parked.load(std::memory_order_acquire) ||
                               !running_.load(std::memory_order_acquire);
                    });
                }
                me.parked.store(false, std::memory_order_release);

                // Reset backoff on wake
                me.spin_count = 0;
                me.yield_count = 0;
            }

            // Track idle time
            me.idle_cycles_ns.fetch_add(
                100, std::memory_order_relaxed);  // estimated idle quantum
        }
    }

    tl_worker_id = kExternalThread;
    tl_executor = nullptr;
}

// ── Wake helpers ──

void WorkStealingExecutor::wake_one_worker() {
    // Wake the first parked worker
    for (auto& ws : workers_) {
        if (ws->parked.load(std::memory_order_acquire)) {
            ws->parked.store(false, std::memory_order_release);
            {
                std::lock_guard lock(ws->park_mutex);
            }
            ws->park_cv.notify_one();
            return;
        }
    }
}

void WorkStealingExecutor::wake_worker(size_t worker_id) {
    auto& ws = *workers_[worker_id];
    if (ws.parked.load(std::memory_order_acquire)) {
        ws.parked.store(false, std::memory_order_release);
        {
            std::lock_guard lock(ws.park_mutex);
        }
        ws.park_cv.notify_one();
    }
}

// ── Stats ──

WorkStealingExecutor::Stats WorkStealingExecutor::stats() const {
    Stats s;

    for (const auto& ws : workers_) {
        s.local_pops += ws->local_pops.load(std::memory_order_relaxed);
        s.tasks_completed += ws->tasks_completed.load(std::memory_order_relaxed);
        s.local_steals += ws->steals_from_me.load(std::memory_order_relaxed);

        auto busy = ws->busy_cycles_ns.load(std::memory_order_relaxed);
        auto idle = ws->idle_cycles_ns.load(std::memory_order_relaxed);
        auto total = busy + idle;
        if (total > 0) {
            s.utilization += static_cast<double>(busy) / static_cast<double>(total);
        }
        if (ws->parked.load(std::memory_order_relaxed) ||
            busy > 0) {
            s.active_workers++;
        }
    }

    s.utilization /= static_cast<double>(num_workers_);
    return s;
}

}  // namespace quant::infra
