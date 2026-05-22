// disk_persistence.h — Columnar on-disk persistence layer
// Segment file format: [Header][BlockIndex x N][BlockData x N]
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include "cpp/quant/infra/coroutine.h"

#include "cpp/quant/storage/column_block.h"
#include "cpp/quant/storage/segment_index.h"

namespace quant::network { class CoIouring; }

namespace quant::storage {

using infra::CoTask;

// ── Sync mode for write durability ──
enum class SyncMode : uint8_t {
    kAsync = 0,    // No fsync (default, fastest)
    kSync = 1,     // fsync after every write
    kPeriodic = 2, // fsync periodically via flush()
};

// ── On-disk segment file constants ──
constexpr uint32_t kSegmentMagic = 0x51554E54;  // "QUNT"
constexpr uint32_t kSegmentVersion = 1;
constexpr size_t   kSegmentHeaderSize = 64;
constexpr size_t   kBlockIndexSize = 48;

#pragma pack(push, 1)
struct SegmentHeader {
    uint32_t magic = kSegmentMagic;
    uint32_t version = kSegmentVersion;
    uint32_t symbol_len;
    char     symbol[28];       // fixed-size symbol buffer
    uint8_t  data_type;
    uint32_t num_blocks;
    int64_t  segment_begin_ts; // min timestamp across all blocks
    int64_t  segment_end_ts;   // max timestamp across all blocks
    uint8_t  reserved[3];
};
static_assert(sizeof(SegmentHeader) == kSegmentHeaderSize);

struct BlockIndexEntry {
    DataField field;
    ColumnBlock::Codec codec;
    uint32_t row_count;
    uint32_t compressed_size;
    uint64_t offset;           // byte offset from file start
    int64_t  min_ts;
    int64_t  max_ts;
    uint8_t  reserved[14];
};
static_assert(sizeof(BlockIndexEntry) == kBlockIndexSize);
#pragma pack(pop)

// ── DiskPersistence: manages segment files on disk ──
class DiskPersistence {
public:
    explicit DiskPersistence(std::filesystem::path data_dir,
                             SyncMode sync_mode = SyncMode::kAsync);
    ~DiskPersistence();

    DiskPersistence(const DiskPersistence&) = delete;
    DiskPersistence& operator=(const DiskPersistence&) = delete;

    // Write a group of column blocks as one segment file
    // Returns: segment filename (without directory)
    std::string write_segment(std::string_view symbol, uint8_t data_type,
                              const std::vector<ColumnBlock>& blocks,
                              int64_t begin_ts, int64_t end_ts);

    // Read all blocks from a segment file
    std::vector<ColumnBlock> read_segment(std::string_view filename) const;

    // Read blocks matching field + time range from a segment
    std::vector<ColumnBlock> read_segment_filtered(
        std::string_view filename, DataField field,
        int64_t range_begin, int64_t range_end) const;

    // List all segment files for a given symbol+type
    std::vector<std::string> list_segments(std::string_view symbol,
                                           uint8_t data_type) const;

    // Query segment blocks via in-memory index (O(log n) instead of O(n) directory scan)
    // Returns SegmentMeta entries matching symbol+data_type+field that overlap [begin_ts, end_ts]
    std::vector<SegmentMeta> query_index(std::string_view symbol,
                                          uint8_t data_type,
                                          DataField field,
                                          int64_t begin_ts,
                                          int64_t end_ts) const;

    // Read a specific block from a segment file by offset and size
    // More efficient than read_segment when you know the exact block location
    ColumnBlock read_block_at(std::string_view filename,
                              DataField field,
                              ColumnBlock::Codec codec,
                              size_t row_count,
                              size_t offset,
                              size_t compressed_size,
                              int64_t min_ts,
                              int64_t max_ts) const;

    // Delete a segment file
    bool delete_segment(std::string_view filename);

    // Merge small segment files for a given (symbol, data_type) into larger segments.
    // Scans all segment files for this symbol+type via the index, reads segments
    // whose blocks have fewer than min_rows_to_keep rows, and merges them into
    // one segment file. Old files are deleted; the new file is added to the index.
    // Returns the number of merged segment files, or 0 if no compaction was needed.
    size_t compact(std::string_view symbol, uint8_t data_type,
                   size_t min_rows_to_keep = 4096);

    // Flush / sync all pending writes
    void flush();

    const std::filesystem::path& data_dir() const noexcept { return data_dir_; }
    SyncMode sync_mode() const noexcept { return sync_mode_; }

    // Access the segment index for startup build / inspection
    SegmentIndex& index() noexcept { return index_; }
    const SegmentIndex& index() const noexcept { return index_; }

public:
    void set_io_uring(quant::network::CoIouring* ring) noexcept { ring_ = ring; }

    CoTask<std::string> co_write_segment(
        std::string_view symbol, uint8_t data_type,
        const std::vector<ColumnBlock>& blocks,
        int64_t begin_ts, int64_t end_ts);
    CoTask<std::vector<ColumnBlock>> co_read_segment(
        std::string_view filename) const;

private:
    static void do_fsync(int fd);
    std::string segment_filename(std::string_view symbol,
                                  uint8_t data_type,
                                  int64_t begin_ts) const;

    std::filesystem::path data_dir_;
    SyncMode sync_mode_;
    quant::network::CoIouring* ring_{nullptr};
    SegmentIndex index_;  // in-memory segment index for O(log n) queries
};

}  // namespace quant::storage
