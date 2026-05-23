// corporate_action_store.cc — Corporate actions implementation
#include "cpp/quant/storage/corporate_action_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace quant::storage {

// ── File format constants ──
constexpr uint32_t kCorpActMagic = 0x434F5250;  // "CORP"
constexpr uint32_t kCorpActVersion = 1;
constexpr size_t   kCorpActHeaderSize = 64;

#pragma pack(push, 1)
struct CorpActFileHeader {
    uint32_t magic = kCorpActMagic;
    uint32_t version = kCorpActVersion;
    uint32_t num_actions;
    uint8_t  reserved[52];  // padding to 64 bytes
};
static_assert(sizeof(CorpActFileHeader) == kCorpActHeaderSize);
#pragma pack(pop)

CorporateActionStore::CorporateActionStore(Options opts)
    : opts_(std::move(opts)) {}

void CorporateActionStore::add_action(CorporateAction action) {
    auto& vec = data_[action.symbol];
    // Insert sorted by action_date
    auto it = std::upper_bound(vec.begin(), vec.end(), action,
        [](const CorporateAction& a, const CorporateAction& b) {
            return a.action_date < b.action_date;
        });
    vec.insert(it, std::move(action));
}

std::vector<CorporateAction> CorporateActionStore::query_actions(
    const std::string& symbol, int64_t start_date, int64_t end_date) const {
    auto it = data_.find(symbol);
    if (it == data_.end()) return {};

    const auto& vec = it->second;
    std::vector<CorporateAction> result;

    auto lo = std::lower_bound(vec.begin(), vec.end(), start_date,
        [](const CorporateAction& a, int64_t d) { return a.action_date < d; });
    auto hi = std::upper_bound(vec.begin(), vec.end(), end_date,
        [](int64_t d, const CorporateAction& a) { return d < a.action_date; });

    result.insert(result.end(), lo, hi);
    return result;
}

double CorporateActionStore::adjust_price(const std::string& symbol,
                                           int64_t date,
                                           double raw_price) const {
    auto it = data_.find(symbol);
    if (it == data_.end()) return raw_price;

    // Backward adjustment: divide by cumulative adjust_factor
    // for all actions after the given date
    double cumulative = 1.0;
    for (const auto& action : it->second) {
        if (action.action_date <= date) continue;
        cumulative *= action.adjust_factor;
    }
    return raw_price / cumulative;
}

void CorporateActionStore::flush() const {
    auto path = opts_.data_dir / "corp_actions.bin";

    // Count total actions
    uint32_t total = 0;
    for (const auto& [sym, vec] : data_) {
        total += static_cast<uint32_t>(vec.size());
    }
    if (total == 0) return;

    // Ensure directory exists
    if (!std::filesystem::exists(opts_.data_dir)) {
        std::filesystem::create_directories(opts_.data_dir);
    }

    // Calculate file size
    size_t file_size = sizeof(CorpActFileHeader);
    for (const auto& [sym, vec] : data_) {
        for ([[maybe_unused]] const auto& act : vec) {
            uint16_t slen = static_cast<uint16_t>(sym.size());
            file_size += sizeof(uint16_t) + slen +
                         sizeof(int64_t) + sizeof(uint8_t) +
                         sizeof(double) + sizeof(double);
        }
    }

    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    std::vector<uint8_t> buf(file_size);
    size_t pos = 0;

    // Header
    CorpActFileHeader header;
    header.num_actions = total;
    std::memcpy(buf.data() + pos, &header, sizeof(header));
    pos += sizeof(header);

    // Actions
    for (const auto& [sym, vec] : data_) {
        for (const auto& act : vec) {
            uint16_t slen = static_cast<uint16_t>(sym.size());
            std::memcpy(buf.data() + pos, &slen, sizeof(slen));
            pos += sizeof(slen);
            std::memcpy(buf.data() + pos, sym.data(), slen);
            pos += slen;
            std::memcpy(buf.data() + pos, &act.action_date, sizeof(act.action_date));
            pos += sizeof(act.action_date);
            uint8_t type = static_cast<uint8_t>(act.type);
            std::memcpy(buf.data() + pos, &type, sizeof(type));
            pos += sizeof(type);
            std::memcpy(buf.data() + pos, &act.value, sizeof(act.value));
            pos += sizeof(act.value);
            std::memcpy(buf.data() + pos, &act.adjust_factor, sizeof(act.adjust_factor));
            pos += sizeof(act.adjust_factor);
        }
    }

    ssize_t written = ::pwrite(fd, buf.data(), buf.size(), 0);
    ::close(fd);

    if (written < 0 || static_cast<size_t>(written) != buf.size()) {
        std::filesystem::remove(path);
    }
}

void CorporateActionStore::load() {
    auto path = opts_.data_dir / "corp_actions.bin";
    if (!std::filesystem::exists(path)) return;

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return;

    CorpActFileHeader header;
    ssize_t n = ::pread(fd, &header, sizeof(header), 0);
    if (n < 0 || static_cast<size_t>(n) != sizeof(header) ||
        header.magic != kCorpActMagic) {
        ::close(fd);
        return;
    }

    off_t offset = sizeof(header);
    for (uint32_t i = 0; i < header.num_actions; ++i) {
        uint16_t slen;
        n = ::pread(fd, &slen, sizeof(slen), offset);
        if (n < 0 || static_cast<size_t>(n) != sizeof(slen)) break;
        offset += sizeof(slen);

        std::string symbol(slen, '\0');
        n = ::pread(fd, symbol.data(), slen, offset);
        if (n < 0 || static_cast<size_t>(n) != slen) break;
        offset += slen;

        int64_t action_date;
        n = ::pread(fd, &action_date, sizeof(action_date), offset);
        if (n < 0 || static_cast<size_t>(n) != sizeof(action_date)) break;
        offset += sizeof(action_date);

        uint8_t type_raw;
        n = ::pread(fd, &type_raw, sizeof(type_raw), offset);
        if (n < 0 || static_cast<size_t>(n) != sizeof(type_raw)) break;
        offset += sizeof(type_raw);

        double value;
        n = ::pread(fd, &value, sizeof(value), offset);
        if (n < 0 || static_cast<size_t>(n) != sizeof(value)) break;
        offset += sizeof(value);

        double adjust_factor;
        n = ::pread(fd, &adjust_factor, sizeof(adjust_factor), offset);
        if (n < 0 || static_cast<size_t>(n) != sizeof(adjust_factor)) break;
        offset += sizeof(adjust_factor);

        CorporateAction act;
        act.symbol = symbol;
        act.action_date = action_date;
        act.type = static_cast<ActionType>(type_raw);
        act.value = value;
        act.adjust_factor = adjust_factor;
        add_action(std::move(act));
    }

    ::close(fd);
}

size_t CorporateActionStore::num_actions() const noexcept {
    size_t total = 0;
    for (const auto& [sym, vec] : data_) {
        total += vec.size();
    }
    return total;
}

}  // namespace quant::storage
