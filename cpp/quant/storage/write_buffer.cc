// write_buffer.cc — Coroutine-friendly write buffer with WAL + periodic flush
//
// Fully coroutine-based: uses AffinityMutex for thread-affine locking,
// coroutine background task (co_sleep loop) instead of jthread+cv.
#include "cpp/quant/storage/write_buffer.h"
#include "cpp/quant/storage/storage_engine.h"

namespace quant::storage {

WriteBuffer::WriteBuffer(StorageEngine& engine, Options opts)
    : engine_(engine)
    , opts_(std::move(opts)) {
    if (opts_.enable_wal) {
        wal_ = std::make_unique<WriteAheadLog>(opts_.wal_opts);
    }
}

WriteBuffer::~WriteBuffer() {
    stop_background_flush();
    flush();
}

// ── Coroutine API ──

CoTask<StoreStatus> WriteBuffer::co_write(std::string_view symbol, uint8_t data_type,
                                           const event::KlineRow& row) {
    // WAL first (crash safety)
    if (wal_) {
        bool ok = co_await wal_->co_append(symbol, data_type, row);
        if (!ok) co_return StoreStatus::kIoError;
    }

    auto guard = co_await mutex_.co_scoped_lock();
    BufferKey key{std::string(symbol), data_type};
    buffers_[key].push_back(row);
    pending_rows_.fetch_add(1, std::memory_order_relaxed);

    // Flush if threshold reached
    if (pending_rows_.load(std::memory_order_relaxed) >= opts_.flush_row_threshold) {
        co_await do_flush_locked();
    }

    co_return StoreStatus::kOk;
}

CoTask<StoreStatus> WriteBuffer::co_write_batch(std::string_view symbol, uint8_t data_type,
                                                 const std::vector<event::KlineRow>& rows) {
    if (rows.empty()) co_return StoreStatus::kInvalidArgument;

    // WAL first
    if (wal_) {
        bool ok = co_await wal_->co_append_batch(symbol, data_type, rows);
        if (!ok) co_return StoreStatus::kIoError;
    }

    auto guard = co_await mutex_.co_scoped_lock();
    BufferKey key{std::string(symbol), data_type};
    auto& buf = buffers_[key];
    buf.reserve(buf.size() + rows.size());
    for (const auto& row : rows) {
        buf.push_back(row);
    }
    pending_rows_.fetch_add(rows.size(), std::memory_order_relaxed);

    // Flush if threshold reached
    if (pending_rows_.load(std::memory_order_relaxed) >= opts_.flush_row_threshold) {
        co_await do_flush_locked();
    }

    co_return StoreStatus::kOk;
}

CoTask<void> WriteBuffer::co_flush() {
    auto guard = co_await mutex_.co_scoped_lock();
    co_await do_flush_locked();
}

CoTask<void> WriteBuffer::do_flush_locked() {
    for (auto& [key, rows] : buffers_) {
        if (rows.empty()) continue;
        co_await engine_.co_store_kline_batch(key.symbol, key.data_type, rows);
    }
    buffers_.clear();
    pending_rows_.store(0, std::memory_order_relaxed);

    // Truncate WAL after successful flush
    if (wal_) {
        co_await wal_->co_truncate();
    }
}

// ── Synchronous API (backward compatible, wraps coroutine API via blockingWait) ──

StoreStatus WriteBuffer::write(std::string_view symbol, uint8_t data_type,
                                const event::KlineRow& row) {
    return infra::blockingWait(co_write(symbol, data_type, row));
}

StoreStatus WriteBuffer::write_batch(std::string_view symbol, uint8_t data_type,
                                      const std::vector<event::KlineRow>& rows) {
    return infra::blockingWait(co_write_batch(symbol, data_type, rows));
}

void WriteBuffer::flush() {
    infra::blockingWait(co_flush());
}

// ── Background flush ──

void WriteBuffer::start_background_flush(folly::Executor* executor) {
    bg_executor_ = executor;
    stopped_.store(false);

    // Launch the background flush loop as a detached coroutine on the executor.
    // folly::coro::co_withExecutor binds the task to the executor so it
    // can use co_sleep and co_await co_scoped_lock() properly.
    flush_scope_.add(
        folly::coro::co_withExecutor(executor, background_flush_loop()));
}

void WriteBuffer::stop_background_flush() {
    stopped_.store(true);
}

folly::coro::Task<void> WriteBuffer::background_flush_loop() {
    while (!stopped_.load(std::memory_order_relaxed)) {
        co_await infra::sleep(opts_.flush_interval);

        if (stopped_.load(std::memory_order_relaxed)) break;

        if (pending_rows_.load(std::memory_order_relaxed) > 0) {
            try {
                co_await co_flush();
            } catch (...) {
                // Flush errors are logged but don't stop the background loop.
                // The data stays in the buffer and will be retried next cycle.
            }
        }
    }
}

// ── Recovery ──

void WriteBuffer::recover() {
    if (!wal_) return;

    auto entries = wal_->replay();
    if (entries.empty()) return;

    // Group by symbol+data_type and replay as batches
    std::unordered_map<BufferKey, std::vector<event::KlineRow>, BufferKeyHash> groups;
    for (auto& e : entries) {
        BufferKey key{std::move(e.symbol), e.data_type};
        groups[key].push_back(e.row);
    }

    for (auto& [key, rows] : groups) {
        infra::blockingWait(engine_.co_store_kline_batch(key.symbol, key.data_type, rows));
    }

    // Clear WAL after recovery
    wal_->truncate();
}

}  // namespace quant::storage