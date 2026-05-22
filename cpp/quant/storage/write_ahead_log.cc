// write_ahead_log.cc — WAL for crash-safe single-row kline writes
//
// Coroutine-friendly: uses AffinityMutex for thread-affine locking.
// Synchronous API (append/append_batch/truncate) uses try_lock + lock
// for non-coroutine callers. Coroutine API (co_append/co_append_batch/
// co_truncate) uses co_await co_lock().
#include "cpp/quant/storage/write_ahead_log.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace quant::storage {

WriteAheadLog::WriteAheadLog(Options opts)
    : opts_(std::move(opts)) {
    std::filesystem::create_directories(opts_.wal_dir);
    open_current_file();
}

WriteAheadLog::~WriteAheadLog() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool WriteAheadLog::open_current_file() {
    if (fd_ >= 0) {
        ::close(fd_);
    }

    // Find next sequence number
    DIR* dir = ::opendir(opts_.wal_dir.c_str());
    int max_seq = 0;
    if (dir) {
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name.size() > 4 && name.substr(0, 4) == "wal_") {
                try {
                    int seq = std::stoi(name.substr(4));
                    if (seq > max_seq) max_seq = seq;
                } catch (...) {}
            }
        }
        ::closedir(dir);
    }

    wal_seq_ = max_seq + 1;
    std::string path = (opts_.wal_dir / ("wal_" + std::to_string(wal_seq_))).string();
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    bytes_written_ = 0;
    return fd_ >= 0;
}

void WriteAheadLog::rotate_if_needed() {
    size_t max_bytes = opts_.max_wal_size_mb * 1024 * 1024;
    if (bytes_written_ >= max_bytes) {
        open_current_file();
    }
}

bool WriteAheadLog::write_record(std::string_view symbol, uint8_t data_type,
                                  const event::KlineRow& row) {
    if (fd_ < 0) return false;

    WalRecord rec{};
    rec.header.magic = 0x57414C47;
    rec.header.data_type = data_type;
    rec.header.symbol_len = static_cast<uint8_t>(
        std::min(symbol.size(), sizeof(rec.symbol)));
    rec.header.entry_len = sizeof(WalRecord) - sizeof(WalEntryHeader);

    std::memcpy(rec.symbol, symbol.data(), rec.header.symbol_len);
    if (rec.header.symbol_len < sizeof(rec.symbol)) {
        std::memset(rec.symbol + rec.header.symbol_len, 0,
                     sizeof(rec.symbol) - rec.header.symbol_len);
    }
    rec.row = row;

    ssize_t written = ::write(fd_, &rec, sizeof(rec));
    if (written != sizeof(rec)) return false;

    ::fdatasync(fd_);
    bytes_written_ += sizeof(rec);
    rotate_if_needed();
    return true;
}

// ── Synchronous API (for recovery, non-coroutine contexts) ──

bool WriteAheadLog::append(std::string_view symbol, uint8_t data_type,
                            const event::KlineRow& row) {
    // Use try_lock for non-coroutine context. If lock is contended,
    // spin briefly (WAL writes are fast, contention is rare).
    while (!mutex_.try_lock()) {
        std::this_thread::yield();
    }
    bool ok = write_record(symbol, data_type, row);
    mutex_.unlock();
    return ok;
}

bool WriteAheadLog::append_batch(std::string_view symbol, uint8_t data_type,
                                  const std::vector<event::KlineRow>& rows) {
    while (!mutex_.try_lock()) {
        std::this_thread::yield();
    }

    if (fd_ < 0) {
        mutex_.unlock();
        return false;
    }

    for (const auto& row : rows) {
        WalRecord rec{};
        rec.header.magic = 0x57414C47;
        rec.header.data_type = data_type;
        rec.header.symbol_len = static_cast<uint8_t>(
            std::min(symbol.size(), sizeof(rec.symbol)));
        rec.header.entry_len = sizeof(WalRecord) - sizeof(WalEntryHeader);

        std::memcpy(rec.symbol, symbol.data(), rec.header.symbol_len);
        if (rec.header.symbol_len < sizeof(rec.symbol)) {
            std::memset(rec.symbol + rec.header.symbol_len, 0,
                         sizeof(rec.symbol) - rec.header.symbol_len);
        }
        rec.row = row;

        ssize_t written = ::write(fd_, &rec, sizeof(rec));
        if (written != sizeof(rec)) {
            mutex_.unlock();
            return false;
        }
        bytes_written_ += sizeof(rec);
    }

    ::fdatasync(fd_);
    rotate_if_needed();
    mutex_.unlock();
    return true;
}

void WriteAheadLog::truncate() {
    while (!mutex_.try_lock()) {
        std::this_thread::yield();
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    DIR* dir = ::opendir(opts_.wal_dir.c_str());
    if (dir) {
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name.size() > 4 && name.substr(0, 4) == "wal_") {
                std::string path = (opts_.wal_dir / name).string();
                ::unlink(path.c_str());
            }
        }
        ::closedir(dir);
    }

    bytes_written_ = 0;
    open_current_file();
    mutex_.unlock();
}

// ── Coroutine API ──

CoTask<bool> WriteAheadLog::co_append(std::string_view symbol, uint8_t data_type,
                                       const event::KlineRow& row) {
    auto guard = co_await mutex_.co_scoped_lock();
    co_return write_record(symbol, data_type, row);
}

CoTask<bool> WriteAheadLog::co_append_batch(std::string_view symbol, uint8_t data_type,
                                             const std::vector<event::KlineRow>& rows) {
    auto guard = co_await mutex_.co_scoped_lock();

    if (fd_ < 0) co_return false;

    for (const auto& row : rows) {
        WalRecord rec{};
        rec.header.magic = 0x57414C47;
        rec.header.data_type = data_type;
        rec.header.symbol_len = static_cast<uint8_t>(
            std::min(symbol.size(), sizeof(rec.symbol)));
        rec.header.entry_len = sizeof(WalRecord) - sizeof(WalEntryHeader);

        std::memcpy(rec.symbol, symbol.data(), rec.header.symbol_len);
        if (rec.header.symbol_len < sizeof(rec.symbol)) {
            std::memset(rec.symbol + rec.header.symbol_len, 0,
                         sizeof(rec.symbol) - rec.header.symbol_len);
        }
        rec.row = row;

        ssize_t written = ::write(fd_, &rec, sizeof(rec));
        if (written != sizeof(rec)) co_return false;
        bytes_written_ += sizeof(rec);
    }

    ::fdatasync(fd_);
    rotate_if_needed();
    co_return true;
}

CoTask<void> WriteAheadLog::co_truncate() {
    auto guard = co_await mutex_.co_scoped_lock();

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    DIR* dir = ::opendir(opts_.wal_dir.c_str());
    if (dir) {
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name.size() > 4 && name.substr(0, 4) == "wal_") {
                std::string path = (opts_.wal_dir / name).string();
                ::unlink(path.c_str());
            }
        }
        ::closedir(dir);
    }

    bytes_written_ = 0;
    open_current_file();
}

// ── Replay (read-only, no lock needed) ──

std::vector<WriteAheadLog::ReplayedEntry> WriteAheadLog::replay() {
    std::vector<ReplayedEntry> entries;

    DIR* dir = ::opendir(opts_.wal_dir.c_str());
    if (!dir) return entries;

    std::vector<std::string> wal_files;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name.size() > 4 && name.substr(0, 4) == "wal_") {
            wal_files.push_back(name);
        }
    }
    ::closedir(dir);

    std::sort(wal_files.begin(), wal_files.end());

    for (const auto& name : wal_files) {
        std::string path = (opts_.wal_dir / name).string();
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;

        while (true) {
            WalRecord rec{};
            ssize_t n = ::read(fd, &rec, sizeof(rec));
            if (n != sizeof(rec)) break;
            if (rec.header.magic != 0x57414C47) break;

            ReplayedEntry e;
            e.symbol = std::string(rec.symbol, rec.header.symbol_len);
            e.data_type = rec.header.data_type;
            e.row = rec.row;
            entries.push_back(std::move(e));
        }

        ::close(fd);
    }

    return entries;
}

}  // namespace quant::storage
