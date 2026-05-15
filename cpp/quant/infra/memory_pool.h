// memory_pool.h — Tiered memory pool with pmr::memory_resource interface
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace quant::infra {

// ── Small object cache config ──
struct SmallObjectConfig {
    uint32_t max_object_size = 256;       // Objects larger go to central pool
    uint32_t slot_count = 8;             // Size class count (8/16/32/64/128/256)
    uint32_t per_thread_cache_lines = 64; // Thread-local cache lines
};

// ── Memory pool stats ──
struct MemoryPoolStats {
    size_t total_allocated;
    size_t total_freed;
    size_t current_in_use;
    size_t peak_usage;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t cache_hit_count;
    uint64_t cache_miss_count;
    double cache_hit_rate() const noexcept {
        uint64_t total = cache_hit_count + cache_miss_count;
        return total == 0 ? 0.0 : static_cast<double>(cache_hit_count) / total;
    }
};

// ── Main memory pool ──
class QuantMemoryResource : public std::pmr::memory_resource {
public:
    explicit QuantMemoryResource(const SmallObjectConfig& cfg = {});
    ~QuantMemoryResource() override;

    // ── Warmup: pre-allocate memory ──
    void warmup(size_t total_bytes);

    // ── Stats ──
    MemoryPoolStats stats() const noexcept;

    // ── Reset: return all cached objects but keep underlying blocks ──
    void reset() noexcept;

protected:
    void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void* ptr, size_t bytes, size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Convenience allocator ──
template<typename T>
class QuantAllocator {
public:
    using value_type = T;

    explicit QuantAllocator(QuantMemoryResource* mr) noexcept : mr_(mr) {}

    template<typename U>
    QuantAllocator(const QuantAllocator<U>& other) noexcept : mr_(other.resource()) {}

    T* allocate(size_t n) {
        return static_cast<T*>(mr_->allocate(n * sizeof(T), alignof(T)));
    }

    void deallocate(T* ptr, size_t n) noexcept {
        mr_->deallocate(ptr, n * sizeof(T), alignof(T));
    }

    QuantMemoryResource* resource() const noexcept { return mr_; }

    bool operator==(const QuantAllocator& other) const noexcept {
        return mr_ == other.mr_;
    }

    bool operator!=(const QuantAllocator& other) const noexcept {
        return mr_ != other.mr_;
    }

private:
    QuantMemoryResource* mr_;
};

// ── Global singleton ──
QuantMemoryResource& global_memory_resource();

// ── Convenience type aliases ──
template<typename T>
using PmrVector = std::pmr::vector<T>;

template<typename T, typename... Args>
std::unique_ptr<T> make_quant(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

}  // namespace quant::infra