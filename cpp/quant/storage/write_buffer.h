// write_buffer.h — Coroutine-friendly write buffer with WAL + periodic flush
//
// Migration: std::mutex → AffinityMutex for thread-affine locking.
// Background flush uses jthread + cv (kept for simplicity; coroutine
// migration of the background loop requires executor binding).
// Both sync and coroutine APIs are provided.
#pragma once

#include <atomic>
#include <chrono>
#include <stop_token>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/storage/time_series_store.h"
#include "cpp/quant/storage/write_ahead_log.h"

namespace quant::storage {

// Forward declaration to avoid circular include with storage_engine.h
class StorageEngine;

using infra::CoTask;

class WriteBuffer {
public:
    struct Options {
        WriteAheadLog::Options wal_opts;
        size_t flush_row_threshold = 8192;       // flush after this many rows
        std::chrono::seconds flush_interval{5};   // flush after this duration
        bool enable_wal = true;
    };

    WriteBuffer(StorageEngine& engine, Options opts);
    ~WriteBuffer();

    WriteBuffer(const WriteBuffer&) = delete;
    WriteBuffer& operator=(const WriteBuffer&) = delete;

    // ── Synchronous API (backward compatible) ──
    StoreStatus write(std::string_view symbol, uint8_t data_type,
                      const event::KlineRow& row);

    StoreStatus write_batch(std::string_view symbol, uint8_t data_type,
                            const std::vector<event::KlineRow>& rows);

    void flush();
    void recover();

    // ── Coroutine API ──
    CoTask<StoreStatus> co_write(std::string_view symbol, uint8_t data_type,
                                 const event::KlineRow& row);

    CoTask<StoreStatus> co_write_batch(std::string_view symbol, uint8_t data_type,
                                       const std::vector<event::KlineRow>& rows);

    CoTask<void> co_flush();

    // ── Background flush lifecycle ──
    // Note: background flush is auto-started in constructor via jthread.
    // These are kept for future coroutine-based background flush migration.
    void start_background_flush() {}  // No-op: already started
    void stop_background_flush();

    size_t pending_rows() const noexcept { return pending_rows_.load(); }

private:
    struct BufferKey {
        std::string symbol;
        uint8_t data_type;
        bool operator==(const BufferKey& o) const {
            return symbol == o.symbol && data_type == o.data_type;
        }
    };

    struct BufferKeyHash {
        size_t operator()(const BufferKey& k) const {
            return std::hash<std::string>()(k.symbol) ^
                   (std::hash<int>()(static_cast<int>(k.data_type)) << 16);
        }
    };

    void flush_locked();
    CoTask<void> do_flush_locked();
    void background_flush(std::stop_token st);

    StorageEngine& engine_;
    Options opts_;
    std::unique_ptr<WriteAheadLog> wal_;

    infra::AffinityMutex mutex_;
    std::unordered_map<BufferKey, std::vector<event::KlineRow>,
                       BufferKeyHash> buffers_;
    std::atomic<size_t> pending_rows_{0};

    std::jthread flush_thread_;
    std::atomic<bool> stopped_{false};
};

}  // namespace quant::storage
