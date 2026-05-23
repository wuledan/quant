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

WorkStealingExecutor* WorkStealingExecutor::current_executor() {
    return tl_executor;
}

// ── Construction / destruction ──

WorkStealingExecutor::WorkStealingExecutor(
    size_t num_workers, std::string name_prefix)
    : name_prefix_(std::move(name_prefix)) {

    // ── hwloc topology discovery ──
    hwloc_topology_t topo;
    hwloc_topology_init(&topo);
    hwloc_topology_load(topo);

    int numa_count = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_NUMANODE);
    if (numa_count < 1) numa_count = 1;

    size_t per_numa = (num_workers + static_cast<size_t>(numa_count) - 1)
                      / static_cast<size_t>(numa_count);

    workers_.reserve(num_workers);
    size_t total_workers = 0;

    for (int n = 0; n < numa_count && total_workers < num_workers; ++n) {
        hwloc_obj_t node = hwloc_get_obj_by_type(topo, HWLOC_OBJ_NUMANODE, n);

        // Collect physical PU IDs (first PU per core — skip HT siblings)
        std::vector<int> pu_ids;
        unsigned n_pus = hwloc_get_nbobjs_inside_cpuset_by_type(
            topo, node->cpuset, HWLOC_OBJ_PU);
        for (unsigned i = 0; i < n_pus; ++i) {
            hwloc_obj_t pu = hwloc_get_obj_inside_cpuset_by_type(
                topo, node->cpuset, HWLOC_OBJ_PU, i);
            hwloc_obj_t core = hwloc_get_ancestor_obj_by_type(
                topo, HWLOC_OBJ_CORE, pu);
            // Take only the first PU of each physical core
            if (core && pu == core->children[0]) {
                pu_ids.push_back(pu->os_index);
            }
        }

        // Fallback: if no cores found, use all PUs from this NUMA node
        if (pu_ids.empty()) {
            for (unsigned i = 0; i < n_pus; ++i) {
                hwloc_obj_t pu = hwloc_get_obj_inside_cpuset_by_type(
                    topo, node->cpuset, HWLOC_OBJ_PU, i);
                pu_ids.push_back(pu->os_index);
            }
        }

        // Assign workers to this NUMA node
        size_t workers_here = std::min(per_numa,
            num_workers - total_workers);
        if (workers_here == 0) break;

        std::vector<size_t> peers;
        for (size_t w = 0; w < workers_here; ++w) {
            int pu = pu_ids.empty() ? 0
                : pu_ids[(pu_ids.size() * w) / workers_here];
            size_t id = total_workers + w;

            auto ws = std::make_unique<WorkerState>();
            ws->local_deque =
                std::make_unique<ChaseLevDeque<WorkItem>>(kLocalDequeCapacity);
            ws->affine_queue =
                std::make_unique<WorkerState::AffineQueue>();
            ws->worker_id = id;
            ws->hwloc_cpu = pu;
            ws->numa_node = n;
            workers_.push_back(std::move(ws));
            peers.push_back(id);
        }

        numa_peers_.push_back(std::move(peers));
        total_workers += workers_here;
    }

    num_workers_ = total_workers;

    // Build numa_nodes_ vector (per-worker NUMA node lookup)
    numa_nodes_.resize(num_workers_);
    for (size_t i = 0; i < num_workers_; ++i) {
        numa_nodes_[i] = workers_[i]->numa_node;
    }

    hwloc_topology_destroy(topo);
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
        ws->thread = std::thread([this, id = ws->worker_id]() {
            // Bind this thread to its assigned physical CPU
            auto& me = *workers_[id];
            hwloc_topology_t topo;
            hwloc_topology_init(&topo);
            hwloc_topology_load(topo);
            hwloc_cpuset_t set = hwloc_bitmap_alloc();
            hwloc_bitmap_set(set, me.hwloc_cpu);
            hwloc_set_cpubind(topo, set, HWLOC_CPUBIND_THREAD);
            hwloc_bitmap_free(set);
            hwloc_topology_destroy(topo);
            worker_loop(id);
        });
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

    workers_[worker_id]->affine_queue->enqueue(std::move(item));
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

        // ── Step 1: affine queue (MPMC, concurrent-safe) ──
        // Thread-affine submissions (coroutine resumes) have highest priority.
        WorkItem affine_item;
        if (me.affine_queue->try_dequeue(affine_item)) {
            item = std::move(affine_item);
            found = true;
        }

        // ── Step 2: pop own Chase-Lev deque (LIFO, O(1), lock-free) ──
        // Only the owner thread calls push/pop — never a thief.
        if (!found) {
            auto popped = me.local_deque->pop();
            if (popped) {
                item = std::move(*popped);
                found = true;
                me.local_pops.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // ── Step 3: read from global queue ──
        if (!found) {
            WorkItem global_item;
            if (global_queue_.try_dequeue(global_item)) {
                // If this task has affinity to a different worker, redirect
                // via the target's affine queue (NOT local_deque::push —
                // we are not the owner of that Chase-Lev deque).
                if (global_item.target_worker_id != kExternalThread &&
                    global_item.target_worker_id != my_id) {
                    workers_[global_item.target_worker_id]->affine_queue->
                        enqueue(std::move(global_item));
                    // Wake the target worker so it picks up the affine task
                    wake_worker(global_item.target_worker_id);
                    found = false;
                } else {
                    item = std::move(global_item);
                    found = true;
                }
            }
        }

        // ── Step 4: steal from NUMA-local victim (FIFO, lock-free) ──
        // Only steal from workers in the same NUMA node to avoid cross-NUMA
        // cache misses and memory controller contention.
        if (!found) {
            auto& peers = numa_peers_[me.numa_node];
            if (peers.size() > 1) {
                for (size_t attempt = 0; attempt < kStealAttempts; ++attempt) {
                    size_t idx = fast_rand() % peers.size();
                    size_t victim = peers[idx];
                    if (victim == my_id) {
                        idx = (idx + 1) % peers.size();
                        victim = peers[idx];
                    }

                    auto stolen = workers_[victim]->local_deque->steal();
                    if (stolen) {
                        workers_[victim]->steals_from_me.fetch_add(
                            1, std::memory_order_relaxed);
                        // Thread affinity → redirect via target's affine queue
                        if (stolen->target_worker_id != kExternalThread &&
                            stolen->target_worker_id != my_id) {
                            workers_[stolen->target_worker_id]->affine_queue->
                                enqueue(std::move(*stolen));
                            wake_worker(stolen->target_worker_id);
                            found = false;
                            break;
                        }
                        item = std::move(*stolen);
                        found = true;
                        break;
                    }
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
                // parked=true and re-check queue inside the lock to
                // prevent lost-wakeup: if add() enqueued after our
                // last queue check, it will either see parked==true
                // under our lock and wake us, or we see the item now.
                {
                    std::unique_lock lock(me.park_mutex);
                    me.parked.store(true, std::memory_order_release);

                    // Re-check affine and global queues while holding
                    // park_mutex. wake_worker()/wake_one_worker() also take
                    // park_mutex, so this synchronizes with enqueue and
                    // prevents lost-wakeup.
                    WorkItem late_item;
                    if (me.affine_queue->try_dequeue(late_item)) {
                        me.parked.store(false, std::memory_order_release);
                        lock.unlock();
                        me.spin_count = 0;
                        me.yield_count = 0;
                        auto task_start = std::chrono::steady_clock::now();
                        late_item.execute();
                        auto task_end = std::chrono::steady_clock::now();
                        auto busy_ns =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                task_end - task_start).count();
                        me.busy_cycles_ns.fetch_add(
                            static_cast<uint64_t>(busy_ns),
                            std::memory_order_relaxed);
                        me.tasks_completed.fetch_add(
                            1, std::memory_order_relaxed);
                        continue;
                    }
                    if (global_queue_.try_dequeue(late_item)) {
                        me.parked.store(false, std::memory_order_release);
                        lock.unlock();
                        me.spin_count = 0;
                        me.yield_count = 0;
                        auto task_start = std::chrono::steady_clock::now();
                        late_item.execute();
                        auto task_end = std::chrono::steady_clock::now();
                        auto busy_ns =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                task_end - task_start).count();
                        me.busy_cycles_ns.fetch_add(
                            static_cast<uint64_t>(busy_ns),
                            std::memory_order_relaxed);
                        me.tasks_completed.fetch_add(
                            1, std::memory_order_relaxed);
                        continue;
                    }

                    me.park_cv.wait(lock, [&]() {
                        return !me.parked.load(std::memory_order_acquire) ||
                               !running_.load(std::memory_order_acquire);
                    });
                    me.parked.store(false, std::memory_order_release);
                }

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
    for (auto& ws : workers_) {
        std::lock_guard lock(ws->park_mutex);
        if (ws->parked.load(std::memory_order_acquire)) {
            ws->parked.store(false, std::memory_order_release);
            ws->park_cv.notify_one();
            return;
        }
    }
}

void WorkStealingExecutor::wake_worker(size_t worker_id) {
    auto& ws = *workers_[worker_id];
    std::lock_guard lock(ws.park_mutex);
    if (ws.parked.load(std::memory_order_acquire)) {
        ws.parked.store(false, std::memory_order_release);
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
