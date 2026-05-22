// segment_index.cc — In-memory segment index implementation
//
// Coroutine-friendly: uses AffinitySharedMutex for thread-affine
// read-write locking. Sync API wraps coroutine API via blockingWait.
#include "cpp/quant/storage/segment_index.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/storage/disk_persistence.h"

namespace quant::storage {

// ── Internal helpers (caller must hold appropriate lock) ──

void SegmentIndex::insert_sorted(FieldSegmentList& list, SegmentMeta meta) {
    auto it = std::lower_bound(list.begin(), list.end(), meta,
        [](const SegmentMeta& a, const SegmentMeta& b) {
            return a.min_ts < b.min_ts;
        });
    list.insert(it, std::move(meta));
}

size_t SegmentIndex::upper_bound_ts(const FieldSegmentList& list, int64_t ts) {
    size_t lo = 0, hi = list.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (list[mid].min_ts <= ts)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

std::vector<SegmentMeta> SegmentIndex::parse_segment_file(
    const std::string& data_dir, std::string_view filename) const {
    std::filesystem::path fpath = std::filesystem::path(data_dir) / std::string(filename);
    std::ifstream ifs(fpath, std::ios::binary);
    if (!ifs) return {};

    SegmentHeader header;
    ifs.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != kSegmentMagic) return {};

    std::string symbol(header.symbol, header.symbol_len);
    uint8_t data_type = header.data_type;

    std::vector<BlockIndexEntry> block_index(header.num_blocks);
    ifs.read(reinterpret_cast<char*>(block_index.data()),
             block_index.size() * sizeof(BlockIndexEntry));

    std::vector<SegmentMeta> result;
    result.reserve(block_index.size());
    for (const auto& bi : block_index) {
        SegmentMeta meta;
        meta.file_path = std::string(filename);
        meta.field = bi.field;
        meta.codec = bi.codec;
        meta.row_count = bi.row_count;
        meta.compressed_size = bi.compressed_size;
        meta.min_ts = bi.min_ts;
        meta.max_ts = bi.max_ts;
        meta.file_offset = static_cast<size_t>(bi.offset);
        result.push_back(std::move(meta));
    }
    return result;
}

// ── Build (startup scan, no concurrent access) ──

void SegmentIndex::build(const std::string& data_dir) {
    index_.clear();

    if (!std::filesystem::exists(data_dir)) return;

    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        if (!fname.ends_with(".seg")) continue;

        std::filesystem::path fpath = std::filesystem::path(data_dir) / fname;
        std::ifstream ifs(fpath, std::ios::binary);
        if (!ifs) continue;

        SegmentHeader header;
        ifs.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (header.magic != kSegmentMagic) continue;

        std::string symbol(header.symbol, header.symbol_len);
        uint8_t data_type = header.data_type;

        std::vector<BlockIndexEntry> block_index(header.num_blocks);
        ifs.read(reinterpret_cast<char*>(block_index.data()),
                 block_index.size() * sizeof(BlockIndexEntry));

        IndexKey key{symbol, data_type};
        auto& field_map = index_[key];

        for (const auto& bi : block_index) {
            SegmentMeta meta;
            meta.file_path = fname;
            meta.field = bi.field;
            meta.codec = bi.codec;
            meta.row_count = bi.row_count;
            meta.compressed_size = bi.compressed_size;
            meta.min_ts = bi.min_ts;
            meta.max_ts = bi.max_ts;
            meta.file_offset = static_cast<size_t>(bi.offset);

            auto& list = field_map[bi.field];
            insert_sorted(list, std::move(meta));
        }
    }
}

// ── Coroutine API ──

CoTask<std::vector<SegmentMeta>> SegmentIndex::co_query(
    std::string_view symbol, uint8_t data_type,
    DataField field, int64_t begin_ts, int64_t end_ts) const {
    auto lock = co_await rwlock_.co_scoped_shared_lock();

    IndexKey key{std::string(symbol), data_type};
    auto it = index_.find(key);
    if (it == index_.end()) co_return {};

    auto field_it = it->second.find(field);
    if (field_it == it->second.end()) co_return {};

    const auto& list = field_it->second;
    if (list.empty()) co_return {};

    size_t end_idx = upper_bound_ts(list, end_ts);

    std::vector<SegmentMeta> result;
    result.reserve(end_idx / 4 + 1);

    for (size_t i = 0; i < end_idx; ++i) {
        if (list[i].max_ts >= begin_ts) {
            result.push_back(list[i]);
        }
    }

    co_return result;
}

CoTask<void> SegmentIndex::co_add(
    std::string_view symbol, uint8_t data_type, SegmentMeta meta) {
    auto lock = co_await rwlock_.co_scoped_lock();

    IndexKey key{std::string(symbol), data_type};
    auto& list = index_[key][meta.field];
    insert_sorted(list, std::move(meta));
    co_return;
}

CoTask<void> SegmentIndex::co_add_segment(
    const std::string& data_dir, std::string_view filename) {
    auto lock = co_await rwlock_.co_scoped_lock();

    std::filesystem::path fpath = std::filesystem::path(data_dir) / std::string(filename);
    std::ifstream ifs(fpath, std::ios::binary);
    if (!ifs) co_return;

    SegmentHeader header;
    ifs.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != kSegmentMagic) co_return;

    std::string symbol(header.symbol, header.symbol_len);
    uint8_t data_type = header.data_type;

    std::vector<BlockIndexEntry> block_index(header.num_blocks);
    ifs.read(reinterpret_cast<char*>(block_index.data()),
             block_index.size() * sizeof(BlockIndexEntry));

    IndexKey key{symbol, data_type};
    auto& field_map = index_[key];

    for (const auto& bi : block_index) {
        SegmentMeta meta;
        meta.file_path = std::string(filename);
        meta.field = bi.field;
        meta.codec = bi.codec;
        meta.row_count = bi.row_count;
        meta.compressed_size = bi.compressed_size;
        meta.min_ts = bi.min_ts;
        meta.max_ts = bi.max_ts;
        meta.file_offset = static_cast<size_t>(bi.offset);

        auto& list = field_map[bi.field];
        insert_sorted(list, std::move(meta));
    }
    co_return;
}

CoTask<void> SegmentIndex::co_remove_file(
    std::string_view symbol, uint8_t data_type, std::string_view filename) {
    auto lock = co_await rwlock_.co_scoped_lock();

    IndexKey key{std::string(symbol), data_type};
    auto it = index_.find(key);
    if (it == index_.end()) co_return;

    for (auto& [field, list] : it->second) {
        std::erase_if(list, [&](const SegmentMeta& m) {
            return m.file_path == filename;
        });
    }
    co_return;
}

CoTask<void> SegmentIndex::co_remove(std::string_view symbol, uint8_t data_type) {
    auto lock = co_await rwlock_.co_scoped_lock();

    IndexKey key{std::string(symbol), data_type};
    index_.erase(key);
    co_return;
}

CoTask<size_t> SegmentIndex::co_size() const {
    auto lock = co_await rwlock_.co_scoped_shared_lock();

    size_t total = 0;
    for (const auto& [key, field_map] : index_) {
        for (const auto& [field, list] : field_map) {
            total += list.size();
        }
    }
    co_return total;
}

CoTask<void> SegmentIndex::co_clear() {
    auto lock = co_await rwlock_.co_scoped_lock();
    index_.clear();
    co_return;
}

// ── Synchronous API (backward compatible) ──

std::vector<SegmentMeta> SegmentIndex::query(
    std::string_view symbol, uint8_t data_type,
    DataField field, int64_t begin_ts, int64_t end_ts) const {
    return infra::blockingWait(co_query(symbol, data_type, field, begin_ts, end_ts));
}

void SegmentIndex::add(std::string_view symbol, uint8_t data_type, SegmentMeta meta) {
    infra::blockingWait(co_add(symbol, data_type, std::move(meta)));
}

void SegmentIndex::add_segment(const std::string& data_dir, std::string_view filename) {
    infra::blockingWait(co_add_segment(data_dir, filename));
}

void SegmentIndex::remove_file(std::string_view symbol, uint8_t data_type,
                               std::string_view filename) {
    infra::blockingWait(co_remove_file(symbol, data_type, filename));
}

void SegmentIndex::remove(std::string_view symbol, uint8_t data_type) {
    infra::blockingWait(co_remove(symbol, data_type));
}

size_t SegmentIndex::size() const {
    return infra::blockingWait(co_size());
}

void SegmentIndex::clear() {
    infra::blockingWait(co_clear());
}

}  // namespace quant::storage