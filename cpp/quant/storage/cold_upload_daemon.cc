// cold_upload_daemon.cc — Cold segment upload daemon implementation
#include "cpp/quant/storage/cold_upload_daemon.h"

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <unordered_map>

#include "cpp/quant/storage/time_series_store.h"

namespace quant::storage {

ColdUploadDaemon::ColdUploadDaemon(DiskPersistence& disk, RemoteStorage& remote,
                                   Options opts)
    : disk_(disk), remote_(remote), opts_(std::move(opts)) {}

// ── Find cold segments ──

std::vector<ColdUploadDaemon::ColdSegment>
ColdUploadDaemon::find_cold_segments() const {
    std::vector<ColdSegment> result;

    const auto& data_dir = disk_.data_dir();
    if (!std::filesystem::exists(data_dir)) return result;

    auto now = std::filesystem::file_time_type::clock::now();
    auto threshold = std::chrono::hours(24 * opts_.cold_threshold_days);

    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (!path.extension().string().ends_with(".seg")) continue;

        // Check mtime coldness
        auto mtime = std::filesystem::last_write_time(path);
        if (now - mtime < threshold) continue;  // still warm

        // Read just the header to extract symbol + data_type + time range
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;

        SegmentHeader header;
        ssize_t n = ::pread(fd, &header, sizeof(header), 0);
        ::close(fd);
        if (n < 0 || static_cast<size_t>(n) != sizeof(header)) continue;
        if (header.magic != kSegmentMagic) continue;

        std::string symbol(header.symbol, header.symbol_len);
        ColdSegment cs;
        cs.file_path = path.filename().string();
        cs.symbol = std::move(symbol);
        cs.data_type = header.data_type;
        cs.min_ts = header.segment_begin_ts;
        cs.max_ts = header.segment_end_ts;
        result.push_back(std::move(cs));
    }

    return result;
}

// ── Single scan + upload pass ──

CoTask<void> ColdUploadDaemon::scan_and_upload() {
    auto cold_list = find_cold_segments();
    if (cold_list.empty()) co_return;

    // Group rows by (symbol, data_type, year) for upload_kline
    struct GroupKey {
        std::string symbol;
        uint8_t data_type;
        int year;

        bool operator==(const GroupKey& o) const {
            return symbol == o.symbol && data_type == o.data_type && year == o.year;
        }
    };
    struct GroupKeyHash {
        size_t operator()(const GroupKey& k) const {
            return std::hash<std::string>()(k.symbol) ^
                   (static_cast<size_t>(k.data_type) << 16) ^
                   (static_cast<size_t>(k.year) << 24);
        }
    };

    std::unordered_map<GroupKey, std::vector<event::KlineRow>, GroupKeyHash> groups;

    for (const auto& cs : cold_list) {
        // Read full segment data
        auto blocks = disk_.read_segment(cs.file_path);
        if (blocks.empty()) continue;

        auto rows = TimeSeriesStore::blocks_to_rows(blocks);
        if (rows.empty()) continue;

        // Determine year from the midpoint timestamp
        int64_t mid_ts = cs.min_ts + (cs.max_ts - cs.min_ts) / 2;
        int year = RemoteStorage::year_from_timestamp(mid_ts);

        GroupKey key{cs.symbol, cs.data_type, year};
        auto& vec = groups[key];
        vec.insert(vec.end(), std::make_move_iterator(rows.begin()),
                   std::make_move_iterator(rows.end()));
    }

    // Upload each group
    for (auto& [key, rows] : groups) {
        remote_.upload_kline(key.symbol, key.data_type, key.year, rows);
    }

    // Optionally delete local cold segments
    if (opts_.remove_after_upload) {
        for (const auto& cs : cold_list) {
            disk_.delete_segment(cs.file_path);
        }
    }
}

// ── Main loop ──

CoTask<void> ColdUploadDaemon::run() {
    while (true) {
        co_await scan_and_upload();
        co_await infra::sleep(opts_.scan_interval);
    }
}

}  // namespace quant::storage
