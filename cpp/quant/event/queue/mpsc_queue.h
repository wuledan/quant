// mpsc_queue.h — Multi-Producer Single-Consumer lock-free queue
// Based on Dmitry Vyukov's MPSC linked-list based queue design.
#pragma once

#include <atomic>
#include <memory>
#include <optional>

namespace quant::event {

// ── Lock-free MPSC queue (intrusive linked list) ──
template<typename T>
class MPSCQueue {
public:
    MPSCQueue() {
        auto* stub = new Node();
        head_.store(stub, std::memory_order_relaxed);
        tail_ = stub;
    }

    ~MPSCQueue() {
        T item;
        while (dequeue(item)) {}
        delete tail_;  // the last stub
    }

    // Disable copy/move
    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    // ── Push (multi-producer, lock-free) ──
    void enqueue(T value) {
        auto* node = new Node(std::move(value));
        Node* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    // ── Pop (single-consumer) ──
    bool dequeue(T& item) {
        Node* t = tail_;
        Node* n = t->next.load(std::memory_order_acquire);
        if (n == nullptr) {
            return false;
        }
        item = std::move(n->value);
        tail_ = n;
        delete t;
        return true;
    }

    // ── Check if empty (approximate, single-consumer safe) ──
    bool empty() const noexcept {
        return tail_->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    struct Node {
        T value;
        std::atomic<Node*> next{nullptr};

        Node() = default;
        explicit Node(T val) : value(std::move(val)) {}
    };

    // Head is written by producers, read by consumer
    std::atomic<Node*> head_;
    // Tail is only accessed by the consumer (single-consumer)
    Node* tail_;
};

}  // namespace quant::event