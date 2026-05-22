// write_ahead_log.h — WAL for crash-safe single-row kline writes
//
// Coroutine-friendly: uses AffinityMutex for thread-affine locking.
// append/append_batch/truncate return CoTask for coroutine chains.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/infra/coroutine.h"

namespace quant::storage {

using infra::CoTask;

#pragma pack(push, 1)
struct WalEntryHeader {
    uint32_t magic = 0x57414C47;  // "WALG"
    uint16_t entry_len;           // total entry bytes after header
    uint8_t  data_type;           // quant::event::DataType
    uint8_t  symbol_len;
};

struct WalRecord {
    WalEntryHeader header;
    char     symbol[28];
    event::KlineRow row;
};
static_assert(sizeof(WalRecord) == 8 + 28 + 48);  // header + symbol + row
#pragma pack(pop)

class WriteAheadLog {
public:
    struct Options {
        std::filesystem::path wal_dir;
        size_t max_wal_size_mb = 64;  // rotate WAL after this size
    };

    explicit WriteAheadLog(Options opts);
    ~WriteAheadLog();

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    // ── Coroutine-friendly API ──
    // Append a single kline row to WAL (fsync for durability)
    CoTask<bool> co_append(std::string_view symbol, uint8_t data_type,
                           const event::KlineRow& row);

    // Append a batch of kline rows (single fsync)
    CoTask<bool> co_append_batch(std::string_view symbol, uint8_t data_type,
                                 const std::vector<event::KlineRow>& rows);

    // Truncate WAL after successful recovery/flush
    CoTask<void> co_truncate();

    // ── Synchronous API (for recovery, non-coroutine contexts) ──
    bool append(std::string_view symbol, uint8_t data_type,
                const event::KlineRow& row);

    bool append_batch(std::string_view symbol, uint8_t data_type,
                      const std::vector<event::KlineRow>& rows);

    void truncate();

    // Replay all committed entries from WAL files (no lock needed, read-only)
    struct ReplayedEntry {
        std::string symbol;
        uint8_t data_type;
        event::KlineRow row;
    };
    std::vector<ReplayedEntry> replay();

    size_t bytes_written() const noexcept { return bytes_written_; }

private:
    bool open_current_file();
    void rotate_if_needed();

    // Internal: write a single record to fd_ (caller must hold lock)
    bool write_record(std::string_view symbol, uint8_t data_type,
                      const event::KlineRow& row);

    Options opts_;
    infra::AffinityMutex mutex_;
    int fd_ = -1;
    size_t bytes_written_ = 0;
    int wal_seq_ = 0;
};

}  // namespace quant::storage
