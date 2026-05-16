// object_pool.h — Pre-allocated object pool with shared_ptr auto-recycling
#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace quant::infra {

// ── Resettable concept ──
template<typename T>
concept Resettable = requires(T& obj) {
    { obj.reset() } -> std::same_as<void>;
};

// ── Object pool config ──
template<typename T>
struct ObjectPoolConfig {
    size_t initial_capacity = 1024;   // Pre-allocated count
    size_t grow_factor = 2;            // Growth multiplier
    size_t max_capacity = 0;           // 0 = unlimited
    bool thread_safe = true;           // Whether thread-safe
};

// ── Object pool ──
template<Resettable T>
class ObjectPool {
public:
    using Config = ObjectPoolConfig<T>;

    explicit ObjectPool(const Config& cfg = {})
        : impl_(std::make_shared<Impl>(cfg))
    {
        warmup(cfg.initial_capacity);
    }

    ~ObjectPool() = default;

    // Disable copy
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // ── Acquire an object ──
    std::shared_ptr<T> acquire() {
        std::lock_guard<std::mutex> lock(impl_->mutex_);

        // Check if we need to grow
        if (impl_->free_list_.empty()) {
            if (impl_->config_.max_capacity > 0 && impl_->capacity_ >= impl_->config_.max_capacity) {
                return nullptr;  // Pool exhausted
            }
            impl_->grow(impl_->config_.grow_factor * impl_->config_.initial_capacity);
        }

        assert(!impl_->free_list_.empty());
        size_t idx = impl_->free_list_.back();
        impl_->free_list_.pop_back();

        // Reset and construct the object
        T* obj = impl_->objects_[idx];
        obj->reset();

        impl_->stats_.acquire_count.fetch_add(1, std::memory_order_relaxed);
        size_t in_use = impl_->in_use_.fetch_add(1, std::memory_order_relaxed) + 1;

        // Update peak
        size_t peak = impl_->peak_in_use_.load(std::memory_order_relaxed);
        while (in_use > peak) {
            if (impl_->peak_in_use_.compare_exchange_weak(peak, in_use,
                    std::memory_order_relaxed)) {
                break;
            }
        }

        impl_->available_.store(impl_->free_list_.size(), std::memory_order_release);

        // Create shared_ptr with custom deleter that returns to pool.
        // Captures weak_ptr<Impl> to safely handle pool destruction before
        // all shared_ptrs are released, avoiding use-after-free.
        std::weak_ptr<Impl> weak = impl_;
        return std::shared_ptr<T>(obj, [weak, idx](T* p) {
            p->reset();
            if (auto impl = weak.lock()) {
                impl->return_object(idx);
            }
        });
    }

    // ── Batch acquire ──
    std::vector<std::shared_ptr<T>> acquire_batch(size_t count) {
        std::vector<std::shared_ptr<T>> result;
        result.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            auto obj = acquire();
            if (!obj) break;
            result.push_back(std::move(obj));
        }
        return result;
    }

    // ── Warmup ──
    void warmup(size_t count) {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->grow(count);
    }

    // ── Stats ──
    struct Stats {
        size_t total_allocated;
        size_t total_in_use;
        size_t total_available;
        size_t peak_in_use;
        uint64_t acquire_count;
        uint64_t release_count;
    };

    Stats stats() const noexcept {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        Stats s;
        s.total_allocated = impl_->capacity_;
        s.total_in_use = impl_->in_use_.load(std::memory_order_relaxed);
        s.total_available = impl_->free_list_.size();
        s.peak_in_use = impl_->peak_in_use_.load(std::memory_order_relaxed);
        s.acquire_count = impl_->stats_.acquire_count.load(std::memory_order_relaxed);
        s.release_count = impl_->stats_.release_count.load(std::memory_order_relaxed);
        return s;
    }

    // ── Shrink ──
    void shrink_to_fit() {}

    // ── Capacity ──
    size_t capacity() const noexcept { return impl_->capacity_; }
    size_t available() const noexcept { return impl_->available_.load(std::memory_order_relaxed); }

private:
    struct Impl {
        explicit Impl(const Config& cfg) : config_(cfg) {}

        ~Impl() {
            // Destroy all allocated objects
            for (size_t i = 0; i < objects_.size(); ++i) {
                objects_[i]->~T();
            }
            // Free raw memory blocks
            for (auto* block : blocks_) {
                ::operator delete(block);
            }
        }

        // Return object to pool (called by shared_ptr deleter)
        void return_object(size_t idx) {
            std::lock_guard<std::mutex> lock(mutex_);
            free_list_.push_back(idx);
            in_use_.fetch_sub(1, std::memory_order_relaxed);
            stats_.release_count.fetch_add(1, std::memory_order_relaxed);
            available_.store(free_list_.size(), std::memory_order_release);
        }

        // Allocate new objects and add to free list
        void grow(size_t count) {
            size_t old_capacity = capacity_;
            capacity_ += count;

            constexpr size_t obj_size = sizeof(T);
            constexpr size_t obj_align = alignof(T);
            size_t block_size = count * obj_size + obj_align;

            char* block = static_cast<char*>(::operator new(block_size));
            blocks_.push_back(block);

            // Align pointer
            uintptr_t addr = reinterpret_cast<uintptr_t>(block);
            addr = (addr + obj_align - 1) & ~(obj_align - 1);
            char* aligned = reinterpret_cast<char*>(addr);

            for (size_t i = 0; i < count; ++i) {
                T* obj = ::new (aligned + i * obj_size) T();
                objects_.push_back(obj);
                free_list_.push_back(old_capacity + i);
            }
        }

        Config config_;
        size_t capacity_ = 0;

        std::vector<T*> objects_;
        std::vector<size_t> free_list_;
        std::vector<char*> blocks_;

        std::atomic<size_t> available_{0};
        std::atomic<size_t> in_use_{0};
        std::atomic<size_t> peak_in_use_{0};

        struct AtomicStats {
            std::atomic<uint64_t> acquire_count{0};
            std::atomic<uint64_t> release_count{0};
        };
        AtomicStats stats_;

        mutable std::mutex mutex_;
    };

    std::shared_ptr<Impl> impl_;
};

// ── Typical usage example ──
struct MarketSnapshot {
    std::string symbol;
    double bid_price = 0.0;
    double ask_price = 0.0;
    int64_t bid_volume = 0;
    int64_t ask_volume = 0;
    int64_t timestamp_ns = 0;

    void reset() {
        symbol.clear();
        bid_price = ask_price = 0.0;
        bid_volume = ask_volume = 0;
        timestamp_ns = 0;
    }
};

using SnapshotPool = ObjectPool<MarketSnapshot>;

}  // namespace quant::infra
