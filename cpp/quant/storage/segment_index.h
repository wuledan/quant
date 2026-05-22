// segment_index.h — In-memory index for segment files, eliminating O(n) directory traversal
// Built at startup by scanning .seg file headers; updated on write/compaction
//
// Coroutine-friendly: uses AffinitySharedMutex for thread-affine
// read-write locking. Both sync and coroutine APIs are provided.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/storage/column_block.h"

namespace quant::storage {

using infra::CoTask;

// ── SegmentMeta: metadata for a single block within a segment file ──
struct SegmentMeta {
    std::string file_path;          // segment filename (relative to data_dir)
    DataField field;                // column field (kOpen, kClose, etc.)
    ColumnBlock::Codec codec;       // compression codec
    size_t row_count;               // number of rows in this block
    size_t compressed_size;         // size of compressed data in bytes
    int64_t min_ts;                 // minimum timestamp in this block
    int64_t max_ts;                 // maximum timestamp in this block
    size_t file_offset;             // byte offset within the segment file
};

// ── SegmentIndex: O(1) lookup for segment blocks by (symbol, data_type, field, time_range) ──
//
// Data structure:
//   Key = (symbol, data_type) → per-field list of SegmentMeta sorted by min_ts
//   Query: binary search for time range overlap → O(log n) per field
//
// Thread safety: AffinitySharedMutex for concurrent reads, exclusive on write
class SegmentIndex {
public:
    // Build index by scanning data_dir/*.seg files at startup
    // Reads only the header + block index of each file (no block data)
    void build(const std::string& data_dir);

    // ── Coroutine API (preferred) ──

    CoTask<std::vector<SegmentMeta>> co_query(
        std::string_view symbol, uint8_t data_type,
        DataField field, int64_t begin_ts, int64_t end_ts) const;

    CoTask<void> co_add(std::string_view symbol, uint8_t data_type, SegmentMeta meta);

    CoTask<void> co_add_segment(const std::string& data_dir, std::string_view filename);

    CoTask<void> co_remove_file(std::string_view symbol, uint8_t data_type,
                                std::string_view filename);

    CoTask<void> co_remove(std::string_view symbol, uint8_t data_type);

    CoTask<size_t> co_size() const;

    CoTask<void> co_clear();

    // ── Synchronous API (backward compatible) ──

    // Query: find all segments matching symbol+data_type that overlap [begin_ts, end_ts]
    // Returns SegmentMeta entries sorted by min_ts
    std::vector<SegmentMeta> query(std::string_view symbol,
                                    uint8_t data_type,
                                    DataField field,
                                    int64_t begin_ts,
                                    int64_t end_ts) const;

    // Add a new segment block (called when a new .seg file is written)
    void add(std::string_view symbol, uint8_t data_type, SegmentMeta meta);

    // Add all blocks from a newly written segment file
    // Reads the file header + block index to extract metadata
    void add_segment(const std::string& data_dir, std::string_view filename);

    // Remove all segment entries for a specific file
    // Used after compaction replaces old segments with a merged one
    void remove_file(std::string_view symbol, uint8_t data_type,
                     std::string_view filename);

    // Remove all segments for a symbol+data_type
    // Used when a full compaction replaces all old segments
    void remove(std::string_view symbol, uint8_t data_type);

    // Total number of indexed block entries
    size_t size() const;

    // Clear the index
    void clear();

private:
    // Composite key for (symbol, data_type)
    struct IndexKey {
        std::string symbol;
        uint8_t data_type;

        bool operator==(const IndexKey& o) const {
            return symbol == o.symbol && data_type == o.data_type;
        }
    };

    struct IndexKeyHash {
        size_t operator()(const IndexKey& k) const {
            return std::hash<std::string>()(k.symbol) ^
                   (static_cast<size_t>(k.data_type) << 24);
        }
    };

    // Per-field segment list, sorted by min_ts for binary search
    using FieldSegmentList = std::vector<SegmentMeta>;

    // Map: field → sorted list of SegmentMeta
    using FieldMap = std::unordered_map<DataField, FieldSegmentList>;

    // Main index: (symbol, data_type) → field → sorted segment list
    std::unordered_map<IndexKey, FieldMap, IndexKeyHash> index_;

    // Thread-affine RW lock: concurrent reads, exclusive writes
    mutable infra::AffinitySharedMutex rwlock_;

    // Internal: insert a SegmentMeta into the correct field list, maintaining sort by min_ts
    void insert_sorted(FieldSegmentList& list, SegmentMeta meta);

    // Internal: binary search for the first entry with min_ts > ts
    static size_t upper_bound_ts(const FieldSegmentList& list, int64_t ts);

    // Internal: parse a single .seg file and return its block metadata
    std::vector<SegmentMeta> parse_segment_file(
        const std::string& data_dir, std::string_view filename) const;

};

}  // namespace quant::storage