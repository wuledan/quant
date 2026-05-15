// thread_pool.cc — Work-Stealing thread pool implementation
#include "cpp/quant/infra/thread_pool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <numeric>
#include <random>
#include <stop_token>

namespace quant::infra {

// ── Per-worker local deque with locking ──
// In a production system this would be replaced by a lock-free deque
// (e.g. boost::lockfree::deque or a Chase-Lev work-stealing deque).
class WorkStealingQueue {
public:
    bool try_push(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= max_size_) return false;
        queue_.push_back(std::move(task));
        return true;
    }

    bool try_pop(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        task = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    bool try_steal(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        task = std::move(queue_.back());
        queue_.pop_back();
        return true;
    }

    void set_max_size(size_t sz) { max_size_ = sz; }
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<Task> queue_;
    size_t max_size_ = 256;
};

// ── Global overflow queue (MPSC) ──
class GlobalQueue {
public:
    void push(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }

    bool try_pop(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        task = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<Task> queue_;
};

// ── ThreadPool::Impl ──
class ThreadPool::Impl {
public:
    explicit Impl(const ThreadPoolConfig& cfg)
        : config_(cfg)
        , random_engine_(std::random_device{}())
    {
        if (config_.worker_count == 0) {
            config_.worker_count = static_cast<uint32_t>(std::thread::hardware_concurrency());
        }
        if (config_.worker_count == 0) {
            config_.worker_count = 4;  // Fallback
        }
    }

    ~Impl() { stop(); }

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;  // Already running
        }

        workers_.reserve(config_.worker_count);
        local_queues_.reserve(config_.worker_count);

        for (uint32_t i = 0; i < config_.worker_count; ++i) {
            local_queues_.push_back(std::make_unique<WorkStealingQueue>());
            local_queues_.back()->set_max_size(config_.local_queue_capacity);
        }

        for (uint32_t i = 0; i < config_.worker_count; ++i) {
            workers_.emplace_back(&Impl::worker_loop, this, i);
        }
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;  // Already stopped
        }

        // Notify all workers to wake up and check the stop flag
        for (uint32_t i = 0; i < config_.worker_count; ++i) {
            cv_[i % cv_.size()].notify_all();
        }
        global_cv_.notify_all();

        // Wait for all workers to finish
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    void force_stop() {
        running_.store(false, std::memory_order_relaxed);
        stop();
    }

    void enqueue_task(Task task) {
        if (!running_.load(std::memory_order_relaxed)) {
            throw std::runtime_error("ThreadPool is not running");
        }

        stats_.tasks_submitted.fetch_add(1, std::memory_order_relaxed);

        // Round-robin: try to push to a local queue
        uint32_t idx = next_worker_.fetch_add(1, std::memory_order_relaxed)
                       % config_.worker_count;

        if (config_.policy == SchedulePolicy::RoundRobin) {
            if (!local_queues_[idx]->try_push(std::move(task))) {
                global_queue_.push(std::move(task));
                stats_.queue_overflow_count.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (config_.policy == SchedulePolicy::LeastLoaded) {
            // Find the least loaded queue
            size_t min_load = local_queues_[idx]->size();
            for (uint32_t i = 0; i < config_.worker_count; ++i) {
                auto load = local_queues_[i]->size();
                if (load < min_load) {
                    min_load = load;
                    idx = i;
                }
            }
            if (!local_queues_[idx]->try_push(std::move(task))) {
                global_queue_.push(std::move(task));
                stats_.queue_overflow_count.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            // WorkStealing: push to the current worker's local queue or global
            if (!local_queues_[idx]->try_push(std::move(task))) {
                global_queue_.push(std::move(task));
                stats_.queue_overflow_count.fetch_add(1, std::memory_order_relaxed);
            }
        }

        global_cv_.notify_one();
    }

    Stats stats() const noexcept {
        return Stats{
            stats_.tasks_submitted.load(std::memory_order_relaxed),
            stats_.tasks_completed.load(std::memory_order_relaxed),
            stats_.tasks_stolen.load(std::memory_order_relaxed),
            stats_.queue_overflow_count.load(std::memory_order_relaxed),
        };
    }

    uint32_t worker_count() const noexcept { return config_.worker_count; }
    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

private:
    void worker_loop(uint32_t worker_id) {
        while (running_.load(std::memory_order_relaxed)) {
            Task task;

            // 1. Try local queue first
            if (local_queues_[worker_id]->try_pop(task)) {
                execute_task(task, worker_id);
                continue;
            }

            // 2. Try global queue
            if (global_queue_.try_pop(task)) {
                execute_task(task, worker_id);
                continue;
            }

            // 3. Try stealing from other workers (WorkStealing policy)
            if (config_.policy == SchedulePolicy::WorkStealing) {
                bool stole = false;
                for (uint32_t attempt = 0; attempt < config_.steal_attempts; ++attempt) {
                    uint32_t victim = random_distribution_(random_engine_)
                                      % config_.worker_count;
                    if (victim == worker_id) continue;

                    if (local_queues_[victim]->try_steal(task)) {
                        stats_.tasks_stolen.fetch_add(1, std::memory_order_relaxed);
                        execute_task(task, worker_id);
                        stole = true;
                        break;
                    }
                }
                if (stole) continue;
            }

            // 4. No task found — wait
            std::unique_lock<std::mutex> lock(global_mutex_);
            global_cv_.wait_for(lock, std::chrono::microseconds(100),
                               [this]() { return !running_.load(std::memory_order_relaxed)
                                                || global_queue_.size() > 0; });
        }
    }

    void execute_task(Task& task, uint32_t /*worker_id*/) {
        task.execute();
        stats_.tasks_completed.fetch_add(1, std::memory_order_relaxed);
    }

    ThreadPoolConfig config_;
    std::vector<std::thread> workers_;
    std::vector<std::unique_ptr<WorkStealingQueue>> local_queues_;
    GlobalQueue global_queue_;

    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_worker_{0};

    // Stats
    struct AtomicStats {
        std::atomic<uint64_t> tasks_submitted{0};
        std::atomic<uint64_t> tasks_completed{0};
        std::atomic<uint64_t> tasks_stolen{0};
        std::atomic<uint64_t> queue_overflow_count{0};
    };
    AtomicStats stats_;

    // Synchronization
    std::mutex global_mutex_;
    std::condition_variable global_cv_;
    std::array<std::condition_variable, 16> cv_;  // Per-worker notification

    // Random for work-stealing
    std::mt19937 random_engine_;
    std::uniform_int_distribution<uint32_t> random_distribution_;
};

// ── Constructor / Destructor ──
ThreadPool::ThreadPool(const ThreadPoolConfig& cfg)
    : impl_(std::make_unique<Impl>(cfg)) {}

ThreadPool::~ThreadPool() = default;

void ThreadPool::start() { impl_->start(); }
void ThreadPool::stop() noexcept { impl_->stop(); }
void ThreadPool::force_stop() noexcept { impl_->force_stop(); }

ThreadPool::Stats ThreadPool::stats() const noexcept { return impl_->stats(); }
uint32_t ThreadPool::worker_count() const noexcept { return impl_->worker_count(); }
bool ThreadPool::is_running() const noexcept { return impl_->is_running(); }

void ThreadPool::enqueue_task(Task task) {
    impl_->enqueue_task(std::move(task));
}

ThreadPool& default_thread_pool() {
    static ThreadPool pool(ThreadPoolConfig{});
    static bool initialized = false;
    if (!initialized) {
        pool.start();
        initialized = true;
    }
    return pool;
}

}  // namespace quant::infra