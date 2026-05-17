// disk_persistence.cc — DiskPersistence implementation
#include "cpp/quant/storage/disk_persistence.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace quant::storage {

DiskPersistence::DiskPersistence(std::filesystem::path data_dir, SyncMode sync_mode)
    : data_dir_(std::move(data_dir)), sync_mode_(sync_mode) {
    if (!std::filesystem::exists(data_dir_)) {
        std::filesystem::create_directories(data_dir_);
    }
}

DiskPersistence::~DiskPersistence() = default;

std::string DiskPersistence::segment_filename(
    std::string_view symbol, uint8_t data_type, int64_t begin_ts) const {
    return std::string(symbol) + "_" + std::to_string(data_type)
         + "_" + std::to_string(begin_ts) + ".seg";
}

std::string DiskPersistence::write_segment(
    std::string_view symbol, uint8_t data_type,
    const std::vector<ColumnBlock>& blocks,
    int64_t begin_ts, int64_t end_ts) {
    if (blocks.empty()) return {};

    std::string fname = segment_filename(symbol, data_type, begin_ts);
    std::filesystem::path fpath = data_dir_ / fname;

    std::ofstream ofs(fpath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("Cannot open segment file for writing: "
                                 + fpath.string());
    }

    // 1. Write header
    SegmentHeader header{};
    size_t slen = std::min(symbol.size(), sizeof(header.symbol) - 1);
    std::memcpy(header.symbol, symbol.data(), slen);
    header.symbol[slen] = '\0';
    header.symbol_len = static_cast<uint32_t>(slen);
    header.data_type = data_type;
    header.num_blocks = static_cast<uint32_t>(blocks.size());
    header.segment_begin_ts = begin_ts;
    header.segment_end_ts = end_ts;
    ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // 2. Write block index (with placeholder offsets)
    std::vector<BlockIndexEntry> index(blocks.size());
    uint64_t data_offset = sizeof(SegmentHeader)
                         + blocks.size() * sizeof(BlockIndexEntry);
    for (size_t i = 0; i < blocks.size(); ++i) {
        index[i].field = blocks[i].field();
        index[i].codec = blocks[i].codec();
        index[i].row_count = static_cast<uint32_t>(blocks[i].row_count());
        index[i].compressed_size = static_cast<uint32_t>(blocks[i].compressed_size());
        index[i].offset = data_offset;
        index[i].min_ts = blocks[i].min_timestamp();
        index[i].max_ts = blocks[i].max_timestamp();
        data_offset += blocks[i].compressed_size();
    }
    ofs.write(reinterpret_cast<const char*>(index.data()),
              index.size() * sizeof(BlockIndexEntry));

    // 3. Write compressed block data
    for (const auto& block : blocks) {
        const auto& data = block.data();
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  data.size());
    }

    ofs.close();

    if (sync_mode_ == SyncMode::kSync) {
        // Open with POSIX fd for fsync
        int fd = ::open(fpath.c_str(), O_RDONLY);
        if (fd >= 0) {
            do_fsync(fd);
            ::close(fd);
        }
    }

    return fname;
}

std::vector<ColumnBlock> DiskPersistence::read_segment(
    std::string_view filename) const {
    std::filesystem::path fpath = data_dir_ / filename;
    std::ifstream ifs(fpath, std::ios::binary);
    if (!ifs) return {};

    // Read header
    SegmentHeader header;
    ifs.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != kSegmentMagic) return {};

    // Read index
    std::vector<BlockIndexEntry> index(header.num_blocks);
    ifs.read(reinterpret_cast<char*>(index.data()),
             index.size() * sizeof(BlockIndexEntry));

    // Read each block
    std::vector<ColumnBlock> result;
    result.reserve(index.size());
    for (const auto& entry : index) {
        std::vector<uint8_t> comp_data(entry.compressed_size);
        ifs.seekg(static_cast<std::streamoff>(entry.offset));
        ifs.read(reinterpret_cast<char*>(comp_data.data()), comp_data.size());

        result.emplace_back(entry.field, entry.codec,
                            entry.row_count, std::move(comp_data),
                            entry.min_ts, entry.max_ts);
    }

    return result;
}

std::vector<ColumnBlock> DiskPersistence::read_segment_filtered(
    std::string_view filename, DataField field,
    int64_t range_begin, int64_t range_end) const {
    std::filesystem::path fpath = data_dir_ / filename;
    std::ifstream ifs(fpath, std::ios::binary);
    if (!ifs) return {};

    SegmentHeader header;
    ifs.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != kSegmentMagic) return {};

    // Quick range check
    if (header.segment_end_ts < range_begin ||
        header.segment_begin_ts > range_end) {
        return {};
    }

    std::vector<BlockIndexEntry> index(header.num_blocks);
    ifs.read(reinterpret_cast<char*>(index.data()),
             index.size() * sizeof(BlockIndexEntry));

    std::vector<ColumnBlock> result;
    for (const auto& entry : index) {
        // Filter by field and time range
        if (entry.field != field) continue;
        if (entry.max_ts < range_begin || entry.min_ts > range_end) continue;

        std::vector<uint8_t> comp_data(entry.compressed_size);
        ifs.seekg(static_cast<std::streamoff>(entry.offset));
        ifs.read(reinterpret_cast<char*>(comp_data.data()), comp_data.size());

        result.emplace_back(entry.field, entry.codec,
                            entry.row_count, std::move(comp_data),
                            entry.min_ts, entry.max_ts);
    }

    return result;
}

std::vector<std::string> DiskPersistence::list_segments(
    std::string_view symbol, uint8_t data_type) const {
    std::vector<std::string> result;
    std::string prefix = std::string(symbol) + "_" + std::to_string(data_type);

    if (!std::filesystem::exists(data_dir_)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        if (fname.ends_with(".seg") &&
            fname.substr(0, prefix.size()) == prefix) {
            result.push_back(fname);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

bool DiskPersistence::delete_segment(std::string_view filename) {
    std::filesystem::path fpath = data_dir_ / filename;
    return std::filesystem::remove(fpath);
}

void DiskPersistence::flush() {
    if (sync_mode_ == SyncMode::kPeriodic) {
        // fsync all segment files in data directory
        if (!std::filesystem::exists(data_dir_)) return;
        for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
            if (!entry.is_regular_file() || !entry.path().string().ends_with(".seg")) continue;
            int fd = ::open(entry.path().c_str(), O_RDONLY);
            if (fd >= 0) {
                do_fsync(fd);
                ::close(fd);
            }
        }
    }
}

void DiskPersistence::do_fsync(int fd) {
#ifdef _WIN32
    _commit(fd);
#else
    ::fsync(fd);
#endif
}

}  // namespace quant::storage
