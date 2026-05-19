// global_executor.h -- Global WorkStealingExecutor singleton
//
// Provides a process-wide singleton for the WorkStealingExecutor that all
// components (TCP, WebSocket, disk I/O, etc.) share. This is the bootstrap
// point for the coroutine scheduler — must be initialized before any
// coroutine-heavy subsystem starts.
//
// Thread safety: instance() is a C++11 thread-safe Meyer's singleton.
// init() uses an atomic flag so only the first caller succeeds.
#pragma once

#include "work_stealing_executor.h"

#include <atomic>
#include <memory>

namespace quant::network {

// ── GlobalExecutor ──
//
// Singleton that owns and manages the WorkStealingExecutor.
//
// Usage:
//   auto& ex = GlobalExecutor::instance();
//   if (!ex.init(std::thread::hardware_concurrency())) {
//       // handle failure
//   }
//   quant::infra::WorkStealingExecutor* worker = ex.executor();

class GlobalExecutor {
public:
    // ── Access ──
    static GlobalExecutor& instance();

    // ── Lifecycle ──
    // Create and start the WorkStealingExecutor with the given worker count.
    // If num_workers == 0, defaults to std::thread::hardware_concurrency().
    // Returns true on success, false on failure (logs via quant logger).
    // Safe to call multiple times — only the first call initializes.
    bool init(size_t num_workers = 0);

    // Stop all workers and release the executor.
    void shutdown();

    // ── Accessors ──
    quant::infra::WorkStealingExecutor* executor() noexcept {
        return executor_.get();
    }

    bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

private:
    GlobalExecutor() = default;
    ~GlobalExecutor();

    GlobalExecutor(const GlobalExecutor&) = delete;
    GlobalExecutor& operator=(const GlobalExecutor&) = delete;

    std::unique_ptr<quant::infra::WorkStealingExecutor> executor_;
    std::atomic<bool> initialized_{false};
};

}  // namespace quant::network
