#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace quant::infra {

// Circular array for Chase-Lev deque.
// Indices wrap via modulo; capacity must be a power of two for correctness
// when the array is shared between grow() and concurrent steal().
template<typename T>
class CircularArray {
public:
    explicit CircularArray(size_t capacity)
        : capacity_(capacity), items_(capacity) {}

    size_t capacity() const noexcept { return capacity_; }

    const T& load(int64_t i) const noexcept {
        return items_[static_cast<size_t>(i) % capacity_];
    }

    // Move-out variant for move-only types.
    T take(int64_t i) noexcept {
        return std::move(items_[static_cast<size_t>(i) % capacity_]);
    }

    void store(int64_t i, T item) noexcept {
        items_[static_cast<size_t>(i) % capacity_] = std::move(item);
    }

    // Create a new array with double capacity, copying elements in [top, bottom).
    std::unique_ptr<CircularArray<T>> grow(int64_t top, int64_t bottom) {
        auto new_array = std::make_unique<CircularArray<T>>(capacity_ * 2);
        for (int64_t i = top; i < bottom; ++i) {
            new_array->store(i, take(i));
        }
        return new_array;
    }

private:
    size_t capacity_;
    std::vector<T> items_;
};

// Chase-Lev work-stealing deque.
// Owner thread: push() and pop() from bottom (LIFO).
// Thief threads: steal() from top (FIFO).
//
// Reference: "Dynamic Circular Work-Stealing Deque" — Chase & Lev, SPAA 2005.
template<typename T>
class ChaseLevDeque {
public:
    explicit ChaseLevDeque(size_t capacity = 256)
        : top_(0), bottom_(0),
          array_(std::make_unique<CircularArray<T>>(capacity).release()) {}

    ~ChaseLevDeque() {
        delete array_.load(std::memory_order_relaxed);
    }

    // Non-copyable, non-movable
    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

    // Owner only: push item to bottom.
    void push(T item) {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_acquire);
        auto* a = array_.load(std::memory_order_relaxed);

        if (static_cast<size_t>(b - t) >= a->capacity()) {
            auto old = a;
            auto new_array = old->grow(t, b);
            auto* new_ptr = new_array.release();
            array_.store(new_ptr, std::memory_order_release);
            delete old;
            a = new_ptr;
        }

        a->store(b, std::move(item));
        bottom_.store(b + 1, std::memory_order_release);
    }

    // Owner only: pop item from bottom (LIFO).
    std::optional<T> pop() {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        auto* a = array_.load(std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        int64_t t = top_.load(std::memory_order_relaxed);
        std::optional<T> result;

        if (t <= b) {
            result = a->take(b);
            if (t == b) {
                // Last item — race with steal
                if (!top_.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    result = std::nullopt;
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
        } else {
            bottom_.store(b + 1, std::memory_order_relaxed);
        }

        return result;
    }

    // Any thread: steal item from top (FIFO).
    std::optional<T> steal() {
        int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t b = bottom_.load(std::memory_order_acquire);

        if (t >= b) {
            return std::nullopt;
        }

        auto* a = array_.load(std::memory_order_consume);
        T item = a->take(t);

        if (!top_.compare_exchange_strong(t, t + 1,
                std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return std::nullopt;
        }

        return item;
    }

    size_t size() const noexcept {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return b > t ? static_cast<size_t>(b - t) : 0;
    }

    bool empty() const noexcept {
        return size() == 0;
    }

private:
    std::atomic<int64_t> top_;
    std::atomic<int64_t> bottom_;
    std::atomic<CircularArray<T>*> array_;
};

}  // namespace quant::infra