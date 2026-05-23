// cold_upload_daemon.h — Background coroutine that uploads cold segments to MinIO
//
// Periodically scans DiskPersistence's data directory for .seg files whose
// mtime exceeds cold_threshold_days. Cold segments are read, converted to
// KlineRow vectors, and uploaded to RemoteStorage grouped by
// (symbol, data_type, year). Optionally deletes local copies after upload.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/storage/disk_persistence.h"
#include "cpp/quant/storage/remote_storage.h"

namespace quant::storage {

using infra::CoTask;

class ColdUploadDaemon {
public:
    struct Options {
        std::chrono::seconds scan_interval{3600};  // every hour
        int64_t cold_threshold_days = 30;          // 30 days no access = cold
        bool remove_after_upload = false;
    };

    ColdUploadDaemon(DiskPersistence& disk, RemoteStorage& remote, Options opts);

    // Main scan loop: scan → upload → sleep → repeat
    // Run on a WorkStealingExecutor via co_withExecutor + .start().detach()
    CoTask<void> run();

    // Exposed for testing: returns metadata for segments cold enough to upload
    struct ColdSegment {
        std::string file_path;
        std::string symbol;
        uint8_t data_type;
        int64_t min_ts;
        int64_t max_ts;
    };
    std::vector<ColdSegment> find_cold_segments() const;

private:
    CoTask<void> scan_and_upload();

    DiskPersistence& disk_;
    RemoteStorage& remote_;
    Options opts_;
};

}  // namespace quant::storage
