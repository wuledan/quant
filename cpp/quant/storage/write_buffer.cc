// write_buffer.cc — Coroutine-friendly write buffer with WAL + periodic flush
//
// Migration from std::mutex → AffinityMutex for thread-affine locking.
// Background flush uses jthread + cv (kept for simplicity; coroutine
// migration of the background loop requires executor binding that
// isn't yet wired up in service_main).
#include "cpp/quant/storage/write_buffer.h"
#include "cpp/quant/storage/storage_engine.h"

#include <chrono>
#include <thread>

namespace quant::storage {

WriteBuffer::WriteBuffer(StorageEngine& engine, Options opts)
    : engine_(engine)
    , opts_(std::move(opts)) {
    if (opts_.enable_wal) {
        wal_ = std::make_unique<WriteAheadLog>(opts_.wal_opts);
    }

    flush_thread_ = std::jthread([this](std::stop_token st) {
        background_flush(std::move(st));
    });
}

WriteBuffer::~WriteBuffer() {
    stopped_.store(true);
    flush_thread_.request_stop();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
    flush();
}

// ── Synchronous API (backward compatible) ──

StoreStatus WriteBuffer::write(std::string_view symbol, uint8_t data_type,
                                const event::KlineRow& row) {
    // WAL first (crash safety)
    if (wal_) {
        if (!wal_->append(symbol, data_type, row)) {
            return StoreStatus::kIoError;
        }
    }

    {
        // Use AffinityMutex with try_lock + yield for sync context
        while (!mutex_.try_lock()) {
            std::this_thread::yield();
        }
        BufferKey key{std::string(symbol), data_type};
        buffers_[key].push_back(row);
        pending_rows_.fetch_add(1, std::memory_order_relaxed);
        mutex_.unlock();
    }

    // Flush if threshold reached
    if (pending_rows_.load(std::memory_order_relaxed) >= opts_.flush_row_threshold) {
        flush();
    }

    return StoreStatus::kOk;
}

StoreStatus WriteBuffer::write_batch(std::string_view symbol, uint8_t data_type,
                                      const std::vector<event::KlineRow>& rows) {
    if (rows.empty()) return StoreStatus::kInvalidArgument;

    // WAL first
    if (wal_) {
        if (!wal_->append_batch(symbol, data_type, rows)) {
            return StoreStatus::kIoError;
        }
    }

    {
        while (!mutex_.try_lock()) {
            std::this_thread::yield();
        }
        BufferKey key{std::string(symbol), data_type};
        auto& buf = buffers_[key];
        buf.reserve(buf.size() + rows.size());
        for (const auto& row : rows) {
            buf.push_back(row);
        }
        pending_rows_.fetch_add(rows.size(), std::memory_order_relaxed);
        mutex_.unlock();
    }

    if (pending_rows_.load(std::memory_order_relaxed) >= opts_.flush_row_threshold) {
        flush();
    }

    return StoreStatus::kOk;
}

void WriteBuffer::flush() {
    while (!mutex_.try_lock()) {
        std::this_thread::yield();
    }
    flush_locked();
}

void WriteBuffer::flush_locked() {
    for (auto& [key, rows] : buffers_) {
        if (rows.empty()) continue;
        engine_.store_kline_batch(key.symbol,
                                   static_cast<event::DataType>(key.data_type),
                                   rows);
    }
    buffers_.clear();
    pending_rows_.store(0, std::memory_order_relaxed);

    // Truncate WAL after successful flush
    if (wal_) {
        wal_->truncate();
    }
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
        engine_.store_kline_batch(key.symbol,
                                   static_cast<event::DataType>(key.data_type),
                                   rows);
    }
    buffers_.clear();
    pending_rows_.store(0, std::memory_order_relaxed);

    // Truncate WAL after successful flush
    if (wal_) {
        co_await wal_->co_truncate();
    }
}

// ── Background flush (jthread, same as original but with AffinityMutex) ──

void WriteBuffer::background_flush(std::stop_token st) {
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(opts_.flush_interval);

        if (st.stop_requested()) break;

        if (pending_rows_.load(std::memory_order_relaxed) > 0) {
            flush();
        }
    }
}

// ── Background flush lifecycle ──

// start_background_flush() is a no-op inline in the header (jthread
// auto-starts in constructor).

void WriteBuffer::stop_background_flush() {
    stopped_.store(true);
    flush_thread_.request_stop();
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
        engine_.store_kline_batch(key.symbol,
                                   static_cast<event::DataType>(key.data_type),
                                   rows);
    }

    // Clear WAL after recovery
    wal_->truncate();
}

}  // namespace quant::storage