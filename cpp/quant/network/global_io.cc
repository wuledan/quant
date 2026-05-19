// global_io.cc -- Global CoIouring singleton implementation
#include "cpp/quant/network/global_io.h"
#include "cpp/quant/network/global_executor.h"

namespace quant::network {

// ════════════════════════════════════════════════════════════════
// Singleton access
// ════════════════════════════════════════════════════════════════

GlobalCoIouring& GlobalCoIouring::instance() {
    static GlobalCoIouring inst;
    return inst;
}

// ════════════════════════════════════════════════════════════════
// Lifecycle
// ════════════════════════════════════════════════════════════════

bool GlobalCoIouring::init() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        // Already initialized — idempotent.
        return true;
    }

    auto& global_ex = GlobalExecutor::instance();
    auto* executor = global_ex.executor();
    if (!executor) {
        initialized_.store(false, std::memory_order_release);
        return false;
    }

    try {
        auto ring = std::make_unique<CoIouring>(1024);
        ring->start(executor);
        io_uring_ = std::move(ring);
        return true;
    } catch (const std::exception&) {
        initialized_.store(false, std::memory_order_release);
        return false;
    }
}

void GlobalCoIouring::shutdown() {
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) {
        return;  // not initialized
    }
    if (io_uring_) {
        io_uring_->stop();
        io_uring_.reset();
    }
}

GlobalCoIouring::~GlobalCoIouring() {
    shutdown();
}

}  // namespace quant::network
