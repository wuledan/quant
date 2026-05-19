// global_executor.cc -- Global WorkStealingExecutor singleton implementation
#include "cpp/quant/network/global_executor.h"

#include <thread>

namespace quant::network {

// ════════════════════════════════════════════════════════════════
// Singleton access
// ════════════════════════════════════════════════════════════════

GlobalExecutor& GlobalExecutor::instance() {
    static GlobalExecutor inst;
    return inst;
}

// ════════════════════════════════════════════════════════════════
// Lifecycle
// ════════════════════════════════════════════════════════════════

bool GlobalExecutor::init(size_t num_workers) {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        // Already initialized — idempotent.
        return true;
    }

    if (num_workers == 0) {
        num_workers = std::thread::hardware_concurrency();
        if (num_workers == 0) {
            num_workers = 2;  // safe fallback
        }
    }

    try {
        auto ex = std::make_unique<quant::infra::WorkStealingExecutor>(
            num_workers, "quant-gbl");
        ex->start();
        executor_ = std::move(ex);
        return true;
    } catch (const std::exception&) {
        initialized_.store(false, std::memory_order_release);
        return false;
    }
}

void GlobalExecutor::shutdown() {
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) {
        return;  // not initialized
    }
    if (executor_) {
        executor_->stop();
        executor_.reset();
    }
}

GlobalExecutor::~GlobalExecutor() {
    shutdown();
}

}  // namespace quant::network
