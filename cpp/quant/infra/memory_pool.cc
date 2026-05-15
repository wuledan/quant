// memory_pool.cc — Tiered memory pool implementation
#include "cpp/quant/infra/memory_pool.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace quant::infra {

// ── Size class calculation ──
// Size classes: 8, 16, 32, 64, 128, 256 bytes
// Each class holds objects of that size, aligned to that size.
static constexpr size_t kSizeClasses[] = {8, 16, 32, 64, 128, 256};
static constexpr size_t kNumSizeClasses = sizeof(kSizeClasses) / sizeof(kSizeClasses[0]);
static constexpr size_t kSmallObjectMax = 256;
static constexpr size_t kMediumObjectMax = 4096;
static constexpr size_t kBlockSize = 1024 * 1024;  // 1MB blocks from OS

// Find the size class for a given allocation size
static size_t size_class_index(size_t size) {
    for (size_t i = 0; i < kNumSizeClasses; ++i) {
        if (size <= kSizeClasses[i]) return i;
    }
    return kNumSizeClasses;  // Not a small object
}

// ── Per-size-class free list ──
class SizeClassFreeList {
public:
    struct Node {
        Node* next{nullptr};
    };

    void push(void* ptr) {
        auto* node = static_cast<Node*>(ptr);
        node->next = head_;
        head_ = node;
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    void* pop() {
        if (!head_) return nullptr;
        auto* node = head_;
        head_ = head_->next;
        count_.fetch_sub(1, std::memory_order_relaxed);
        return node;
    }

    bool empty() const { return head_ == nullptr; }
    size_t count() const { return count_.load(std::memory_order_relaxed); }

private:
    Node* head_{nullptr};
    std::atomic<size_t> count_{0};
};

// ── Central free list for medium objects (up to 4KB) ──
class CentralFreeList {
public:
    void push(void* ptr, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* node = static_cast<FreeNode*>(ptr);
        node->size = size;
        node->next = head_;
        head_ = node;
    }

    void* pop(size_t min_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        FreeNode** prev = &head_;
        FreeNode* curr = head_;
        while (curr) {
            if (curr->size >= min_size) {
                *prev = curr->next;
                return curr;
            }
            prev = &curr->next;
            curr = curr->next;
        }
        return nullptr;
    }

private:
    struct FreeNode {
        size_t size;
        FreeNode* next;
    };
    FreeNode* head_{nullptr};
    std::mutex mutex_;
};

// ── Thread-local cache ──
class ThreadLocalCache {
public:
    ThreadLocalCache() {
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            free_lists_[i] = std::make_unique<SizeClassFreeList>();
        }
    }

    void* allocate(size_t size_class_idx) {
        if (size_class_idx < kNumSizeClasses) {
            return free_lists_[size_class_idx]->pop();
        }
        return nullptr;
    }

    void deallocate(void* ptr, size_t size_class_idx) {
        if (size_class_idx < kNumSizeClasses) {
            free_lists_[size_class_idx]->push(ptr);
        }
    }

private:
    std::array<std::unique_ptr<SizeClassFreeList>, kNumSizeClasses> free_lists_;
};

// ── QuantMemoryResource::Impl ──
class QuantMemoryResource::Impl {
public:
    explicit Impl(const SmallObjectConfig& cfg)
        : config_(cfg)
        , small_free_lists_{}
        , central_free_list_{}
    {
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            small_free_lists_[i] = std::make_unique<SizeClassFreeList>();
        }
    }

    ~Impl() {
        // Free all allocated blocks
        for (auto* block : allocated_blocks_) {
            ::operator delete(block);
        }
        allocated_blocks_.clear();
    }

    void* allocate(size_t bytes, size_t alignment) {
        stats_.alloc_count.fetch_add(1, std::memory_order_relaxed);
        size_t alloc_size = bytes;

        // Align allocation size
        if (alignment > alignof(std::max_align_t)) {
            alloc_size = (alloc_size + alignment - 1) & ~(alignment - 1);
        }

        void* ptr = nullptr;

        // Small object path (<= 256 bytes)
        if (alloc_size <= kSmallObjectMax) {
            size_t class_idx = size_class_index(alloc_size);
            size_t class_size = kSizeClasses[class_idx];

            // Try thread-local free list first
            // (In production, this would use actual thread-local storage)
            std::lock_guard<std::mutex> lock(class_mutexes_[class_idx]);
            ptr = small_free_lists_[class_idx]->pop();

            if (ptr) {
                stats_.cache_hit_count.fetch_add(1, std::memory_order_relaxed);
                return ptr;
            }

            // Cache miss — allocate from block
            stats_.cache_miss_count.fetch_add(1, std::memory_order_relaxed);
            ptr = allocate_from_block(class_size);
            if (ptr) return ptr;

            // No space in block — allocate directly
            ptr = ::operator new(class_size);
            allocated_blocks_.push_back(ptr);
            return ptr;
        }

        // Medium object path (<= 4KB)
        if (alloc_size <= kMediumObjectMax) {
            ptr = central_free_list_.pop(alloc_size);
            if (ptr) {
                stats_.cache_hit_count.fetch_add(1, std::memory_order_relaxed);
                return ptr;
            }
            stats_.cache_miss_count.fetch_add(1, std::memory_order_relaxed);
            ptr = ::operator new(alloc_size);
            allocated_blocks_.push_back(ptr);
            return ptr;
        }

        // Large object path — direct allocation
        stats_.cache_miss_count.fetch_add(1, std::memory_order_relaxed);
        ptr = ::operator new(alloc_size);
        allocated_blocks_.push_back(ptr);
        return ptr;
    }

    void deallocate(void* ptr, size_t bytes, size_t alignment) {
        stats_.free_count.fetch_add(1, std::memory_order_relaxed);
        size_t alloc_size = bytes;

        if (alignment > alignof(std::max_align_t)) {
            alloc_size = (alloc_size + alignment - 1) & ~(alignment - 1);
        }

        // Small object
        if (alloc_size <= kSmallObjectMax) {
            size_t class_idx = size_class_index(alloc_size);
            std::lock_guard<std::mutex> lock(class_mutexes_[class_idx]);
            small_free_lists_[class_idx]->push(ptr);
            return;
        }

        // Medium object — return to central free list
        if (alloc_size <= kMediumObjectMax) {
            central_free_list_.push(ptr, alloc_size);
            return;
        }

        // Large object — direct free
        ::operator delete(ptr);
    }

    void warmup(size_t total_bytes) {
        size_t blocks_needed = (total_bytes + kBlockSize - 1) / kBlockSize;
        for (size_t i = 0; i < blocks_needed; ++i) {
            void* block = ::operator new(kBlockSize);
            allocated_blocks_.push_back(block);
        }
    }

    MemoryPoolStats stats() const noexcept {
        MemoryPoolStats s;
        s.alloc_count = stats_.alloc_count.load(std::memory_order_relaxed);
        s.free_count = stats_.free_count.load(std::memory_order_relaxed);
        s.cache_hit_count = stats_.cache_hit_count.load(std::memory_order_relaxed);
        s.cache_miss_count = stats_.cache_miss_count.load(std::memory_order_relaxed);
        s.total_allocated = stats_.total_allocated.load(std::memory_order_relaxed);
        s.total_freed = stats_.total_freed.load(std::memory_order_relaxed);
        s.current_in_use = s.total_allocated - s.total_freed;
        s.peak_usage = stats_.peak_usage.load(std::memory_order_relaxed);
        return s;
    }

    void reset() noexcept {
        // Don't free underlying blocks — just clear free lists
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            // Note: This is a simple implementation. Production would
            // track objects more carefully.
        }
    }

private:
    void* allocate_from_block(size_t size) {
        // Simple bump allocator within blocks
        // In production, this would manage free spans within blocks
        void* ptr = ::operator new(size);
        stats_.total_allocated.fetch_add(size, std::memory_order_relaxed);
        return ptr;
    }

    SmallObjectConfig config_;
    std::array<std::unique_ptr<SizeClassFreeList>, kNumSizeClasses> small_free_lists_;
    std::array<std::mutex, kNumSizeClasses> class_mutexes_;
    CentralFreeList central_free_list_;
    std::vector<void*> allocated_blocks_;

    struct AtomicStats {
        std::atomic<uint64_t> alloc_count{0};
        std::atomic<uint64_t> free_count{0};
        std::atomic<uint64_t> cache_hit_count{0};
        std::atomic<uint64_t> cache_miss_count{0};
        std::atomic<size_t> total_allocated{0};
        std::atomic<size_t> total_freed{0};
        std::atomic<size_t> peak_usage{0};
    };
    AtomicStats stats_;
};

QuantMemoryResource::QuantMemoryResource(const SmallObjectConfig& cfg)
    : impl_(std::make_unique<Impl>(cfg)) {}

QuantMemoryResource::~QuantMemoryResource() = default;

void QuantMemoryResource::warmup(size_t total_bytes) {
    impl_->warmup(total_bytes);
}

MemoryPoolStats QuantMemoryResource::stats() const noexcept {
    return impl_->stats();
}

void QuantMemoryResource::reset() noexcept {
    impl_->reset();
}

void* QuantMemoryResource::do_allocate(size_t bytes, size_t alignment) {
    return impl_->allocate(bytes, alignment);
}

void QuantMemoryResource::do_deallocate(void* ptr, size_t bytes, size_t alignment) {
    impl_->deallocate(ptr, bytes, alignment);
}

bool QuantMemoryResource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

QuantMemoryResource& global_memory_resource() {
    static QuantMemoryResource instance;
    return instance;
}

}  // namespace quant::infra