// ring_buffer.h — Lock-free ring buffer for event replay
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace quant::event {

template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : capacity_(capacity)
        , buffer_(capacity)
        , head_(0)
        , size_(0) {}

    // ── Push an element (overwrites oldest if full) ──
    void push(T value) {
        size_t idx = (head_ + size_) % capacity_;
        buffer_[idx] = std::move(value);
        if (size_ < capacity_) {
            size_.fetch_add(1, std::memory_order_release);
        } else {
            head_.fetch_add(1, std::memory_order_release);
        }
    }

    // ── Read last N elements into output ──
    size_t read_last(size_t count, std::vector<T>& output) const {
        size_t available = size_.load(std::memory_order_acquire);
        size_t to_read = std::min(count, available);
        if (to_read == 0) return 0;

        output.reserve(output.size() + to_read);
        size_t start = (head_.load(std::memory_order_acquire) + available - to_read) % capacity_;
        for (size_t i = 0; i < to_read; ++i) {
            output.push_back(buffer_[(start + i) % capacity_]);
        }
        return to_read;
    }

    size_t size() const noexcept { return size_.load(std::memory_order_acquire); }
    size_t capacity() const noexcept { return capacity_; }

private:
    size_t capacity_;
    std::vector<T> buffer_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> size_{0};
};

}  // namespace quant::event