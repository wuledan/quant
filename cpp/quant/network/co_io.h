// co_io.h — io_uring-based coroutine I/O layer
//
// Provides CoIouring, a coroutine-friendly wrapper around Linux io_uring.
// Each I/O operation (co_read/co_write/co_recv/co_send/co_accept/co_connect)
// returns a CoTask<T> that suspends the caller until the operation completes.
// Completions are routed back to the caller's worker thread via
// WorkStealingExecutor for thread affinity.
#pragma once

#include "coroutine.h"
#include "work_stealing_executor.h"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

struct io_uring;
struct sockaddr;
struct sockaddr_in;

namespace quant::network {

// ── CoIouring: io_uring wrapper with coroutine support ──
//
// Design:
//   - Single io_uring instance with a dedicated completion-processing thread
//   - Each SQE stores an IoRequest pointer as user_data
//   - Completion thread extracts the IoRequest, stores the result, and
//     resumes the waiting coroutine via WorkStealingExecutor for thread affinity
//   - Awaiter handles the completion-before-suspend race (same pattern as
//     AffinityBaton and WorkStealingExecutor)
//
class CoIouring {
public:
    explicit CoIouring(size_t entries = 1024);
    ~CoIouring();

    CoIouring(const CoIouring&) = delete;
    CoIouring& operator=(const CoIouring&) = delete;

    // ── Lifecycle ──
    // Start the completion processing thread.
    // executor: used to route completions back to the correct worker thread.
    void start(quant::infra::WorkStealingExecutor* executor);

    // Stop the completion processing thread and tear down io_uring.
    void stop();

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // ── Coroutine I/O operations ──

    quant::infra::CoTask<ssize_t> co_read(int fd, void* buf, size_t len,
                                           off_t offset = -1);
    quant::infra::CoTask<ssize_t> co_write(int fd, const void* buf, size_t len,
                                            off_t offset = -1);
    quant::infra::CoTask<ssize_t> co_recv(int fd, void* buf, size_t len,
                                           int flags = 0);
    quant::infra::CoTask<ssize_t> co_send(int fd, const void* buf, size_t len,
                                           int flags = 0);
    quant::infra::CoTask<int> co_accept(int fd, struct sockaddr* addr,
                                         socklen_t* addrlen,
                                         int flags = SOCK_CLOEXEC);
    quant::infra::CoTask<bool> co_connect(int fd,
                                           const struct sockaddr* addr,
                                           socklen_t addrlen);

    // ── Statistics ──
    struct Stats {
        uint64_t submissions{0};
        uint64_t completions{0};
        uint64_t errors{0};
    };
    Stats stats() const noexcept;

private:
    // ── Per-I/O shared state ──
    //
    // State machine (atomic state):
    //   0 = pending (submitted to io_uring, awaiting completion)
    //   1 = completed, waiter not yet suspended
    //   2 = completed, waiter has suspended (completion thread must resume)
    //
    struct IoRequest {
        std::atomic<int> state{0};
        ssize_t result{0};
        int error_code{0};
        std::coroutine_handle<> handle;
        size_t worker_id{quant::infra::WorkStealingExecutor::kExternalThread};
    };

    // ── Common awaiter for all I/O operations ──
    struct IoAwaiter {
        CoIouring& ring;
        IoRequest* req;

        bool await_ready() const noexcept {
            return req->state.load(std::memory_order_acquire) == 1;
        }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            req->handle = h;
            req->worker_id = quant::infra::WorkStealingExecutor::current_worker_id();
            int expected = 0;
            if (!req->state.compare_exchange_strong(
                    expected, 2, std::memory_order_acq_rel)) {
                // expected was 1 (completed before we suspended)
                h.resume();
            }
        }

        ssize_t await_resume() const noexcept {
            return req->result;
        }
    };

    IoRequest* submit_sqe(uint8_t opcode);

    void prep_read(IoRequest* req, int fd, void* buf, size_t len, off_t offset);
    void prep_write(IoRequest* req, int fd, const void* buf, size_t len,
                    off_t offset);
    void prep_recv(IoRequest* req, int fd, void* buf, size_t len, int flags);
    void prep_send(IoRequest* req, int fd, const void* buf, size_t len,
                   int flags);
    void prep_accept(IoRequest* req, int fd, struct sockaddr* addr,
                     socklen_t* addrlen, int flags);
    void prep_connect(IoRequest* req, int fd, const struct sockaddr* addr,
                      socklen_t addrlen);

    void completion_loop();
    void process_completion(void* user_data, ssize_t res, int err);

    struct io_uring* ring_{nullptr};
    std::unique_ptr<std::thread> io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false};
    quant::infra::WorkStealingExecutor* executor_{nullptr};

    std::atomic<uint64_t> submissions_{0};
    std::atomic<uint64_t> completions_{0};
    std::atomic<uint64_t> errors_{0};

    static constexpr size_t kMaxRequests = 4096;
    std::unique_ptr<IoRequest[]> requests_;
    std::atomic<uint32_t> next_idx_{0};
};

}  // namespace quant::network
