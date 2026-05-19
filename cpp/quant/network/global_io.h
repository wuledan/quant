// global_io.h -- Global CoIouring singleton
//
// Provides a process-wide singleton for CoIouring (io_uring + coroutine I/O).
// Must be initialized after GlobalExecutor since CoIouring::start() needs a
// WorkStealingExecutor pointer for completion routing.
//
// Thread safety: instance() is a C++11 thread-safe Meyer's singleton.
// init() uses an atomic flag so only the first caller succeeds.
#pragma once

#include "co_io.h"

#include <atomic>
#include <memory>

namespace quant::network {

// ── GlobalCoIouring ──
//
// Singleton that owns and manages the CoIouring instance.
//
// Usage:
//   auto& io = GlobalCoIouring::instance();
//   if (!io.init()) {
//       // handle failure
//   }
//   // io_uring is now running with completions routed via GlobalExecutor
//   CoIouring* ring = io.io_uring();

class GlobalCoIouring {
public:
    // ── Access ──
    static GlobalCoIouring& instance();

    // ── Lifecycle ──
    // Create the CoIouring and start it with the WorkStealingExecutor
    // from GlobalExecutor. init() must be called after GlobalExecutor::init().
    // Returns true on success, false on failure.
    // Safe to call multiple times — only the first call initializes.
    bool init();

    // Stop the io_uring completion thread and tear down the ring.
    void shutdown();

    // ── Accessors ──
    CoIouring* io_uring() noexcept {
        return io_uring_.get();
    }

    bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

private:
    GlobalCoIouring() = default;
    ~GlobalCoIouring();

    GlobalCoIouring(const GlobalCoIouring&) = delete;
    GlobalCoIouring& operator=(const GlobalCoIouring&) = delete;

    std::unique_ptr<CoIouring> io_uring_;
    std::atomic<bool> initialized_{false};
};

}  // namespace quant::network
