// affinity_baton.cc -- Implementation of executor-routed Baton
//
// The core mechanism:
//   post(executor): atomically drain waiter chain, route each waiter's
//                   continuation via executor.add_to_worker(worker_id, handle)
//   post_direct():  drain and resume inline (no executor routing)

#include "cpp/quant/infra/affinity_baton.h"
#include "cpp/quant/infra/work_stealing_executor.h"

namespace quant::infra {

// ── Worker ID resolution ──
//
// WorkStealingExecutor sets this function pointer during start() to enable
// thread affinity. The default returns SIZE_MAX (no worker affinity),
// which causes post_direct()-style direct resume.

namespace {
    size_t default_worker_id() { return SIZE_MAX; }
}

namespace detail {
    size_t (*get_current_worker_id)() = default_worker_id;
}

size_t AffinityBaton::current_worker_id() {
    return detail::get_current_worker_id();
}

// ── post(executor): routed resume ──

void AffinityBaton::post(WorkStealingExecutor& executor) {
    // Atomically drain the entire waiter chain
    auto* waiters = waiters_.exchange(nullptr, std::memory_order_acq_rel);
    state_.store(State::POSTED, std::memory_order_release);

    resume_chain(waiters, &executor);
}

// ── post_direct(): inline resume (no executor) ──

void AffinityBaton::post_direct() noexcept {
    auto* waiters = waiters_.exchange(nullptr, std::memory_order_acq_rel);
    state_.store(State::POSTED, std::memory_order_release);

    resume_chain(waiters, nullptr);
}

// ── resume_chain: walk the linked list and resume each waiter ──

void AffinityBaton::resume_chain(WaiterNode* waiters,
                                  WorkStealingExecutor* executor) {
    while (waiters) {
        auto* next = waiters->next;
        auto handle = waiters->handle;
        auto worker_id = waiters->worker_id;

        if (executor && worker_id != SIZE_MAX) {
            // Route to the waiter's original worker via executor
            executor->add_to_worker(worker_id, [handle]() mutable {
                handle.resume();
            });
        } else {
            // No executor or no affinity: resume inline
            handle.resume();
        }

        waiters = next;
    }
}

}  // namespace quant::infra
