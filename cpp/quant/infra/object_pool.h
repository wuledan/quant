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
        : config_(cfg)
    {
        warmup(cfg.initial_capacity);
    }

    ~ObjectPool() {
        // Destroy all allocated objects
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < objects_.size(); ++i) {
            objects_[i]->~T();
        }
        // Free raw memory blocks
        for (auto* block : blocks_) {
            ::operator delete(block);
        }
    }

    // Disable copy
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // ── Acquire an object ──
    std::shared_ptr<T> acquire() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check if we need to grow
        if (free_list_.empty()) {
            if (config_.max_capacity > 0 && capacity_ >= config_.max_capacity) {
                return nullptr;  // Pool exhausted
            }
            grow(config_.grow_factor * config_.initial_capacity);
        }

        assert(!free_list_.empty());
        size_t idx = free_list_.back();
        free_list_.pop_back();

        // Reset and construct the object
        T* obj = objects_[idx];
        obj->reset();

        stats_.acquire_count.fetch_add(1, std::memory_order_relaxed);
        size_t in_use = in_use_.fetch_add(1, std::memory_order_relaxed) + 1;

        // Update peak
        size_t peak = peak_in_use_.load(std::memory_order_relaxed);
        while (in_use > peak) {
            if (peak_in_use_.compare_exchange_weak(peak, in_use,
                    std::memory_order_relaxed)) {
                break;
            }
        }

        available_.store(free_list_.size(), std::memory_order_release);

        // Create shared_ptr with custom deleter that returns to pool
        // Capture 'this' and idx for the deleter
        auto* pool = this;
        return std::shared_ptr<T>(obj, [pool, idx](T* p) {
            p->reset();
            pool->return_object(idx);
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
        std::lock_guard<std::mutex> lock(mutex_);
        grow(count);
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
        std::lock_guard<std::mutex> lock(mutex_);
        Stats s;
        s.total_allocated = capacity_;
        s.total_in_use = in_use_.load(std::memory_order_relaxed);
        s.total_available = free_list_.size();
        s.peak_in_use = peak_in_use_.load(std::memory_order_relaxed);
        s.acquire_count = stats_.acquire_count.load(std::memory_order_relaxed);
        s.release_count = stats_.release_count.load(std::memory_order_relaxed);
        return s;
    }

    // ── Shrink ──
    void shrink_to_fit() {
        // Not implemented for safety — could corrupt in-use indices
    }

    // ── Capacity ──
    size_t capacity() const noexcept { return capacity_; }
    size_t available() const noexcept { return available_.load(std::memory_order_relaxed); }

private:
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
        // Must be called with mutex held
        size_t old_capacity = capacity_;
        capacity_ += count;

        // Allocate a new block of raw memory
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

    std::vector<T*> objects_;             // Pointers to constructed objects
    std::vector<size_t> free_list_;       // Available indices
    std::vector<char*> blocks_;           // Raw memory blocks for freeing

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