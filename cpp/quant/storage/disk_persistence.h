// disk_persistence.h — Columnar on-disk persistence layer
// Segment file format: [Header][BlockIndex x N][BlockData x N]
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/quant/storage/column_block.h"

namespace quant::storage {

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
    explicit DiskPersistence(std::filesystem::path data_dir);
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

    // Delete a segment file
    bool delete_segment(std::string_view filename);

    // Flush / sync all pending writes
    void flush();

    const std::filesystem::path& data_dir() const noexcept { return data_dir_; }

private:
    std::string segment_filename(std::string_view symbol,
                                  uint8_t data_type,
                                  int64_t begin_ts) const;

    std::filesystem::path data_dir_;
};

}  // namespace quant::storage
