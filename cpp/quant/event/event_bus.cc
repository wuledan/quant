// event_bus.cc — EventBus implementation with lock-free MPSC async queue
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/queue/mpsc_queue.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace quant::event {

// ── Internal subscription entry ──
struct SubscriptionEntry {
    SubscriptionId id;
    EventTypeId type_id;
    std::unique_ptr<IEventSubscriber> subscriber;
    std::unique_ptr<IEventFilter> filter;
};

// ── Delivery stats ──
struct StatsData {
    std::atomic<uint64_t> total_published{0};
    std::atomic<uint64_t> total_delivered{0};
    std::atomic<int64_t> queue_depth{0};
};

// ── EventBus::Impl ──
struct EventBus::Impl {
public:
    explicit Impl(const EventBus::Options& opts)
        : opts_(opts)
        , next_id_(1)
    {
        subscriptions_.reserve(opts_.subscriber_shard_count);
        for (size_t i = 0; i < opts_.subscriber_shard_count; ++i) {
            subscriptions_.push_back(std::make_unique<Shard>());
        }
    }

    ~Impl() {
        stop();
    }

    SubscriptionId subscribe(EventTypeId type,
                             std::unique_ptr<IEventSubscriber> subscriber,
                             std::unique_ptr<IEventFilter> filter) {
        SubscriptionId id = next_id_.fetch_add(1, std::memory_order_relaxed);
        size_t shard_idx = type % opts_.subscriber_shard_count;

        {
            std::unique_lock lock(subscriptions_[shard_idx]->rwlock);
            subscriptions_[shard_idx]->entries.push_back(SubscriptionEntry{
                .id = id,
                .type_id = type,
                .subscriber = std::move(subscriber),
                .filter = std::move(filter),
            });
        }
        return id;
    }

    void unsubscribe(SubscriptionId id) {
        for (auto& shard : subscriptions_) {
            std::unique_lock lock(shard->rwlock);
            auto it = std::remove_if(shard->entries.begin(), shard->entries.end(),
                [id](const SubscriptionEntry& e) { return e.id == id; });
            if (it != shard->entries.end()) {
                shard->entries.erase(it, shard->entries.end());
                return;
            }
        }
    }

    void publish(std::unique_ptr<Event> event) {
        EventTypeId type = event->event_type_id();
        size_t shard_idx = type % opts_.subscriber_shard_count;

        std::shared_lock lock(subscriptions_[shard_idx]->rwlock);
        for (const auto& entry : subscriptions_[shard_idx]->entries) {
            if (entry.type_id == type) {
                if (entry.filter && !entry.filter->accept(*event)) {
                    continue;
                }
                entry.subscriber->on_event(*event);
                stats_.total_delivered.fetch_add(1, std::memory_order_relaxed);
            }
        }

        stats_.total_published.fetch_add(1, std::memory_order_relaxed);
    }

    void publish_async(std::unique_ptr<Event> event) {
        {
            std::lock_guard<std::mutex> lock(cv_mutex_);
            async_queue_.enqueue(std::move(event));
            stats_.queue_depth.fetch_add(1, std::memory_order_relaxed);
        }
        ensure_async_worker();
        async_cv_.notify_one();
    }

    void replay_history(SubscriptionId id, size_t count) {
        // Replay requires clone() on Event; to be implemented when needed.
        (void)id;
        (void)count;
    }

    Stats stats() const {
        return Stats{
            .total_published = stats_.total_published.load(std::memory_order_relaxed),
            .total_delivered = stats_.total_delivered.load(std::memory_order_relaxed),
            .avg_publish_latency_ns = 0,
            .queue_depth = static_cast<uint64_t>(stats_.queue_depth.load(std::memory_order_relaxed)),
        };
    }

    // ── Explicit lifecycle ──
    void start() {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (async_worker_.joinable()) return;  // already running
        stop_requested_.store(false, std::memory_order_release);
        async_worker_ = std::thread([this] { async_worker_loop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(cv_mutex_);
            stop_requested_.store(true, std::memory_order_release);
        }
        async_cv_.notify_one();

        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (async_worker_.joinable()) {
            async_worker_.join();
        }
    }

private:
    void ensure_async_worker() {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (async_worker_.joinable()) return;
        async_worker_ = std::thread([this] { async_worker_loop(); });
    }

    void async_worker_loop() {
        while (true) {
            std::unique_ptr<Event> event;

            // Drain all available events before waiting
            while (true) {
                std::unique_ptr<Event> ev;
                {
                    // Dequeue under cv_mutex_ so we synchronize with
                    // publish_async which also holds cv_mutex_ when
                    // enqueuing.
                    std::lock_guard<std::mutex> lock(cv_mutex_);
                    if (!async_queue_.dequeue(ev)) {
                        break;
                    }
                }
                if (ev) {
                    publish(std::move(ev));
                    stats_.queue_depth.fetch_sub(1, std::memory_order_relaxed);
                }
            }

            // Wait for new events or stop signal
            {
                std::unique_lock<std::mutex> lock(cv_mutex_);
                async_cv_.wait(lock, [this] {
                    return stop_requested_.load(std::memory_order_acquire)
                           || !async_queue_.empty();
                });

                if (stop_requested_.load(std::memory_order_acquire)) {
                    // Drain remaining events before exiting
                    std::unique_ptr<Event> remaining;
                    while (async_queue_.dequeue(remaining)) {
                        if (remaining) {
                            publish(std::move(remaining));
                            stats_.queue_depth.fetch_sub(1, std::memory_order_relaxed);
                        }
                    }
                    return;
                }
            }
        }
    }

    EventBus::Options opts_;
    std::atomic<SubscriptionId> next_id_;

    // Sharded subscription lists
    struct Shard {
        mutable std::shared_mutex rwlock;
        std::vector<SubscriptionEntry> entries;
    };
    std::vector<std::unique_ptr<Shard>> subscriptions_;

    // Lock-free MPSC async delivery queue
    MPSCQueue<std::unique_ptr<Event>> async_queue_;
    std::mutex cv_mutex_;
    std::condition_variable async_cv_;
    std::atomic<bool> stop_requested_{false};

    std::thread async_worker_;
    std::mutex worker_mutex_;

    // Stats
    StatsData stats_;
};

// ── EventBus public API ──

EventBus::EventBus(const Options& opts)
    : impl_(std::make_unique<Impl>(opts)) {}

EventBus::~EventBus() = default;

SubscriptionId EventBus::subscribe(
    EventTypeId type,
    std::unique_ptr<IEventSubscriber> subscriber,
    std::unique_ptr<IEventFilter> filter) {
    return impl_->subscribe(type, std::move(subscriber), std::move(filter));
}

void EventBus::unsubscribe(SubscriptionId id) {
    impl_->unsubscribe(id);
}

void EventBus::publish(std::unique_ptr<Event> event) {
    impl_->publish(std::move(event));
}

void EventBus::publish_async(std::unique_ptr<Event> event) {
    impl_->publish_async(std::move(event));
}

void EventBus::replay_history(SubscriptionId id, size_t count) {
    impl_->replay_history(id, count);
}

void EventBus::start() {
    impl_->start();
}

void EventBus::stop() {
    impl_->stop();
}

EventBus::Stats EventBus::stats() const {
    return impl_->stats();
}

}  // namespace quant::event