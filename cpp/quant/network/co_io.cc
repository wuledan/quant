// co_io.cc — io_uring coroutine I/O implementation
#include "cpp/quant/network/co_io.h"

#include <cstring>
#include <stdexcept>
#include <system_error>

// Compatibility: newer liburing (2.9+) references BLOCK_URING_CMD_DISCARD
// which is not available in kernel 6.17 headers. Define it to a safe value.
#ifndef BLOCK_URING_CMD_DISCARD
#define BLOCK_URING_CMD_DISCARD 2
#endif

// Use system liburing (compatible with kernel 6.17)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <liburing.h>
#pragma GCC diagnostic pop

#include <sys/socket.h>

namespace quant::network {

// ════════════════════════════════════════════════════════════════
// Construction / destruction
// ════════════════════════════════════════════════════════════════

CoIouring::CoIouring(size_t entries) {
    if (entries > kMaxRequests) entries = kMaxRequests;

    ring_ = new io_uring{};
    int ret = io_uring_queue_init(static_cast<uint32_t>(entries), ring_, 0);
    if (ret < 0) {
        delete ring_;
        ring_ = nullptr;
        throw std::runtime_error("io_uring_queue_init failed: " +
                                 std::string(std::strerror(-ret)));
    }

    requests_ = std::make_unique<IoRequest[]>(kMaxRequests);
}

CoIouring::~CoIouring() {
    stop();
    if (ring_) {
        io_uring_queue_exit(ring_);
        delete ring_;
        ring_ = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════
// Lifecycle
// ════════════════════════════════════════════════════════════════

void CoIouring::start(quant::infra::WorkStealingExecutor* executor) {
    if (running_.exchange(true)) return;
    executor_ = executor;
    stopped_.store(false, std::memory_order_relaxed);
    io_thread_ = std::make_unique<std::thread>([this]() { completion_loop(); });
}

void CoIouring::stop() {
    if (!running_.exchange(false)) return;
    stopped_.store(true, std::memory_order_release);

    if (ring_) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
        if (sqe) {
            io_uring_prep_nop(sqe);
            sqe->user_data = 0;
            io_uring_submit(ring_);
        }
    }

    if (io_thread_ && io_thread_->joinable()) {
        io_thread_->join();
    }
    io_thread_.reset();
    executor_ = nullptr;
}

// ════════════════════════════════════════════════════════════════
// IoRequest allocation
// ════════════════════════════════════════════════════════════════

CoIouring::IoRequest* CoIouring::submit_sqe(uint8_t opcode) {
    auto idx = next_idx_.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kMaxRequests) [[unlikely]] {
        idx = 0;
        next_idx_.store(1, std::memory_order_relaxed);
    }

    IoRequest* req = &requests_[idx];
    req->state.store(0, std::memory_order_relaxed);
    req->result = 0;
    req->error_code = 0;
    req->handle = nullptr;
    return req;
}

// ════════════════════════════════════════════════════════════════
// SQE preparation
// ════════════════════════════════════════════════════════════════

void CoIouring::prep_read(IoRequest* req, int fd, void* buf, size_t len,
                           off_t offset) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) [[unlikely]] {
        req->state.store(1, std::memory_order_release);
        req->result = -ENOMEM;
        return;
    }
    io_uring_prep_read(sqe, fd, buf, len, offset);
    sqe->user_data = reinterpret_cast<uintptr_t>(req);
    io_uring_submit(ring_);
    submissions_.fetch_add(1, std::memory_order_relaxed);
}

void CoIouring::prep_write(IoRequest* req, int fd, const void* buf, size_t len,
                            off_t offset) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) [[unlikely]] {
        req->state.store(1, std::memory_order_release);
        req->result = -ENOMEM;
        return;
    }
    io_uring_prep_write(sqe, fd, buf, len, offset);
    sqe->user_data = reinterpret_cast<uintptr_t>(req);
    io_uring_submit(ring_);
    submissions_.fetch_add(1, std::memory_order_relaxed);
}

void CoIouring::prep_recv(IoRequest* req, int fd, void* buf, size_t len,
                           int flags) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) [[unlikely]] {
        req->state.store(1, std::memory_order_release);
        req->result = -ENOMEM;
        return;
    }
    io_uring_prep_recv(sqe, fd, buf, len, flags);
    sqe->user_data = reinterpret_cast<uintptr_t>(req);
    io_uring_submit(ring_);
    submissions_.fetch_add(1, std::memory_order_relaxed);
}

void CoIouring::prep_send(IoRequest* req, int fd, const void* buf, size_t len,
                           int flags) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) [[unlikely]] {
        req->state.store(1, std::memory_order_release);
        req->result = -ENOMEM;
        return;
    }
    io_uring_prep_send(sqe, fd, buf, len, flags);
    sqe->user_data = reinterpret_cast<uintptr_t>(req);
    io_uring_submit(ring_);
    submissions_.fetch_add(1, std::memory_order_relaxed);
}

void CoIouring::prep_accept(IoRequest* req, int fd, struct sockaddr* addr,
                             socklen_t* addrlen, int flags) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) [[unlikely]] {
        req->state.store(1, std::memory_order_release);
        req->result = -ENOMEM;
        return;
    }
    io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
    sqe->user_data = reinterpret_cast<uintptr_t>(req);
    io_uring_submit(ring_);
    submissions_.fetch_add(1, std::memory_order_relaxed);
}

void CoIouring::prep_connect(IoRequest* req, int fd,
                              const struct sockaddr* addr,
                              socklen_t addrlen) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) [[unlikely]] {
        req->state.store(1, std::memory_order_release);
        req->result = -ENOMEM;
        return;
    }
    io_uring_prep_connect(sqe, fd, addr, addrlen);
    sqe->user_data = reinterpret_cast<uintptr_t>(req);
    io_uring_submit(ring_);
    submissions_.fetch_add(1, std::memory_order_relaxed);
}

// ════════════════════════════════════════════════════════════════
// Completion processing
// ════════════════════════════════════════════════════════════════

void CoIouring::process_completion(void* user_data, ssize_t res, int err) {
    if (user_data == nullptr) return;

    auto* req = static_cast<IoRequest*>(user_data);
    req->result = (err != 0) ? -err : res;
    req->error_code = err;

    completions_.fetch_add(1, std::memory_order_relaxed);
    if (err != 0) {
        errors_.fetch_add(1, std::memory_order_relaxed);
    }

    int expected = 0;
    if (req->state.compare_exchange_strong(expected, 1,
                                           std::memory_order_acq_rel)) {
        // 0→1, waiter hasn't suspended yet
    } else {
        // Was 2, waiter already suspended — resume via executor
        auto handle = req->handle;
        size_t worker_id = req->worker_id;
        if (executor_ && worker_id != quant::infra::WorkStealingExecutor::kExternalThread) {
            executor_->add_to_worker(worker_id, [handle]() { handle.resume(); });
        } else {
            handle.resume();
        }
    }
}

void CoIouring::completion_loop() {
    while (!stopped_.load(std::memory_order_acquire)) {
        struct io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(ring_, &cqe);
        if (ret < 0) {
            if (ret == -EAGAIN || ret == -EINTR) continue;
            break;
        }

        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(ring_, head, cqe) {
            process_completion(
                reinterpret_cast<void*>(cqe->user_data),
                cqe->res,
                cqe->res < 0 ? -static_cast<int>(cqe->res) : 0);
            ++count;
        }
        io_uring_cq_advance(ring_, count);
    }
}

// ════════════════════════════════════════════════════════════════
// Coroutine I/O operations
// ════════════════════════════════════════════════════════════════

quant::infra::CoTask<ssize_t> CoIouring::co_read(int fd, void* buf,
                                                   size_t len,
                                                   off_t offset) {
    IoRequest* req = submit_sqe(IORING_OP_READ);
    prep_read(req, fd, buf, len, offset);
    auto result = co_await IoAwaiter{*this, req};
    co_return result;
}

quant::infra::CoTask<ssize_t> CoIouring::co_write(int fd, const void* buf,
                                                    size_t len,
                                                    off_t offset) {
    IoRequest* req = submit_sqe(IORING_OP_WRITE);
    prep_write(req, fd, buf, len, offset);
    auto result = co_await IoAwaiter{*this, req};
    co_return result;
}

quant::infra::CoTask<ssize_t> CoIouring::co_recv(int fd, void* buf,
                                                   size_t len, int flags) {
    IoRequest* req = submit_sqe(IORING_OP_RECV);
    prep_recv(req, fd, buf, len, flags);
    auto result = co_await IoAwaiter{*this, req};
    co_return result;
}

quant::infra::CoTask<ssize_t> CoIouring::co_send(int fd, const void* buf,
                                                   size_t len, int flags) {
    IoRequest* req = submit_sqe(IORING_OP_SEND);
    prep_send(req, fd, buf, len, flags);
    auto result = co_await IoAwaiter{*this, req};
    co_return result;
}

quant::infra::CoTask<int> CoIouring::co_accept(int fd, struct sockaddr* addr,
                                                 socklen_t* addrlen,
                                                 int flags) {
    IoRequest* req = submit_sqe(IORING_OP_ACCEPT);
    prep_accept(req, fd, addr, addrlen, flags);
    int result = static_cast<int>(co_await IoAwaiter{*this, req});
    co_return result;
}

quant::infra::CoTask<bool> CoIouring::co_connect(int fd,
                                                   const struct sockaddr* addr,
                                                   socklen_t addrlen) {
    IoRequest* req = submit_sqe(IORING_OP_CONNECT);
    prep_connect(req, fd, addr, addrlen);
    auto result = co_await IoAwaiter{*this, req};
    co_return result == 0;
}

// ════════════════════════════════════════════════════════════════
// Statistics
// ════════════════════════════════════════════════════════════════

CoIouring::Stats CoIouring::stats() const noexcept {
    Stats s;
    s.submissions = submissions_.load(std::memory_order_relaxed);
    s.completions = completions_.load(std::memory_order_relaxed);
    s.errors = errors_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace quant::network
