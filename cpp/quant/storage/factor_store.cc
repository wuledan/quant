// factor_store.cc — Cross-sectional factor storage implementation
#include "cpp/quant/storage/factor_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace quant::storage {

FactorStore::FactorStore(Options opts)
    : opts_(std::move(opts)) {}

// ── Write API ──

void FactorStore::put_factor(int64_t date, const std::string& symbol,
                              const std::string& factor_name, double value) {
    data_[factor_name][date][symbol] = value;
}

void FactorStore::put_factor_batch(
    int64_t date, const std::string& factor_name,
    const std::vector<std::pair<std::string, double>>& values) {
    auto& cs = data_[factor_name][date];
    for (const auto& [symbol, value] : values) {
        cs[symbol] = value;
    }
}

// ── Read API ──

std::vector<double> FactorStore::query_factor(
    const std::string& symbol, const std::string& factor_name,
    int64_t start_date, int64_t end_date) const {
    std::vector<double> result;

    auto fit = data_.find(factor_name);
    if (fit == data_.end()) return result;

    const auto& dm = fit->second;
    auto lo = dm.lower_bound(start_date);
    auto hi = dm.upper_bound(end_date);
    for (auto it = lo; it != hi; ++it) {
        auto sit = it->second.find(symbol);
        if (sit != it->second.end()) {
            result.push_back(sit->second);
        }
    }
    return result;
}

std::vector<std::pair<std::string, double>>
FactorStore::query_cross_section(int64_t date,
                                  const std::string& factor_name) const {
    auto fit = data_.find(factor_name);
    if (fit == data_.end()) return {};

    auto dit = fit->second.find(date);
    if (dit == fit->second.end()) return {};

    std::vector<std::pair<std::string, double>> result;
    result.reserve(dit->second.size());
    for (const auto& [sym, val] : dit->second) {
        result.emplace_back(sym, val);
    }
    return result;
}

// ── Persistence ──

size_t FactorStore::num_entries() const noexcept {
    size_t total = 0;
    for (const auto& [fname, dm] : data_) {
        for (const auto& [date, cs] : dm) {
            total += cs.size();
        }
    }
    return total;
}

std::filesystem::path FactorStore::factor_file_path(
    const std::string& factor_name, int year) const {
    auto dir = opts_.data_dir / "factors";
    return dir / (factor_name + "_" + std::to_string(year) + ".fctr");
}

void FactorStore::flush() const {
    // Group data by (factor_name, year)
    struct YearGroup {
        const DateMap* dm;
        std::string factor_name;
        int year;
    };
    std::unordered_map<std::string, std::map<int, const DateMap*>> groups;

    for (const auto& [fname, dm] : data_) {
        // Collect all years present in this factor
        for (const auto& [date, cs] : dm) {
            if (cs.empty()) continue;
            // Convert date (epoch microseconds) to year
            int year = static_cast<int>(date / 1000000 / 86400 / 365.25 + 1970);
            groups[fname][year] = &dm;
            break;  // one year per file is enough, flush_year handles the full dm
        }
    }

    // Ensure factors directory exists
    auto factors_dir = opts_.data_dir / "factors";
    if (!std::filesystem::exists(factors_dir)) {
        std::filesystem::create_directories(factors_dir);
    }

    // Write each (factor_name, year) file
    for (const auto& [fname, year_map] : groups) {
        for (const auto& [year, dm] : year_map) {
            flush_year(fname, year, *dm);
        }
    }
}

void FactorStore::flush_year(const std::string& factor_name, int year,
                              const DateMap& dates) const {
    auto path = factor_file_path(factor_name, year);

    // Count total entries for this year
    int64_t year_start = static_cast<int64_t>(year - 1970) * 365 * 86400 * 1000000LL;
    int64_t year_end = year_start + 366 * 86400 * 1000000LL;

    // Collect dates in range
    std::vector<std::pair<int64_t, const CrossSection*>> year_dates;
    size_t total_entries = 0;
    auto lo = dates.lower_bound(year_start);
    auto hi = dates.lower_bound(year_end);
    for (auto it = lo; it != hi; ++it) {
        if (!it->second.empty()) {
            year_dates.emplace_back(it->first, &it->second);
            total_entries += it->second.size();
        }
    }

    if (year_dates.empty()) return;

    // Calculate file size: header + date_entries + symbol_entries
    size_t file_size = sizeof(FactorFileHeader);
    for (const auto& [date, cs] : year_dates) {
        file_size += sizeof(int64_t) + sizeof(uint32_t);  // date header
        for (const auto& [sym, val] : *cs) {
            file_size += sizeof(uint16_t) + sym.size() + sizeof(double);
        }
    }

    // Write file
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    std::vector<uint8_t> buf(file_size);
    size_t pos = 0;

    // Header
    FactorFileHeader header;
    header.num_dates = static_cast<uint32_t>(year_dates.size());
    std::memcpy(buf.data() + pos, &header, sizeof(header));
    pos += sizeof(header);

    // Date + symbol entries
    for (const auto& [date, cs] : year_dates) {
        std::memcpy(buf.data() + pos, &date, sizeof(date));
        pos += sizeof(date);
        uint32_t n = static_cast<uint32_t>(cs->size());
        std::memcpy(buf.data() + pos, &n, sizeof(n));
        pos += sizeof(n);

        for (const auto& [sym, val] : *cs) {
            uint16_t slen = static_cast<uint16_t>(sym.size());
            std::memcpy(buf.data() + pos, &slen, sizeof(slen));
            pos += sizeof(slen);
            std::memcpy(buf.data() + pos, sym.data(), slen);
            pos += slen;
            std::memcpy(buf.data() + pos, &val, sizeof(val));
            pos += sizeof(val);
        }
    }

    ssize_t written = ::pwrite(fd, buf.data(), buf.size(), 0);
    ::close(fd);

    if (written < 0 || static_cast<size_t>(written) != buf.size()) {
        // Write failed — remove partial file
        std::filesystem::remove(path);
    }
}

void FactorStore::load() {
    auto factors_dir = opts_.data_dir / "factors";
    if (!std::filesystem::exists(factors_dir)) return;

    for (const auto& entry : std::filesystem::directory_iterator(factors_dir)) {
        if (!entry.is_regular_file()) continue;
        if (!entry.path().string().ends_with(".fctr")) continue;
        load_file(entry.path());
    }
}

void FactorStore::load_file(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return;

    // Read header
    FactorFileHeader header;
    ssize_t n = ::pread(fd, &header, sizeof(header), 0);
    if (n < 0 || static_cast<size_t>(n) != sizeof(header) ||
        header.magic != kFactorMagic) {
        ::close(fd);
        return;
    }

    // Extract factor_name from filename: "{factor_name}_{year}.fctr"
    std::string fname = path.stem().string();  // "pe_2025" from "pe_2025.fctr"
    auto underscore = fname.rfind('_');
    if (underscore == std::string::npos) {
        ::close(fd);
        return;
    }
    std::string factor_name = fname.substr(0, underscore);

    // Read all date entries
    off_t offset = sizeof(header);
    for (uint32_t di = 0; di < header.num_dates; ++di) {
        int64_t date;
        n = ::pread(fd, &date, sizeof(date), offset);
        if (n < 0 || static_cast<size_t>(n) != sizeof(date)) break;
        offset += sizeof(date);

        uint32_t num_symbols;
        n = ::pread(fd, &num_symbols, sizeof(num_symbols), offset);
        if (n < 0 || static_cast<size_t>(n) != sizeof(num_symbols)) break;
        offset += sizeof(num_symbols);

        auto& cs = data_[factor_name][date];
        for (uint32_t si = 0; si < num_symbols; ++si) {
            uint16_t slen;
            n = ::pread(fd, &slen, sizeof(slen), offset);
            if (n < 0 || static_cast<size_t>(n) != sizeof(slen)) break;
            offset += sizeof(slen);

            std::string symbol(slen, '\0');
            n = ::pread(fd, symbol.data(), slen, offset);
            if (n < 0 || static_cast<size_t>(n) != slen) break;
            offset += slen;

            double value;
            n = ::pread(fd, &value, sizeof(value), offset);
            if (n < 0 || static_cast<size_t>(n) != sizeof(value)) break;
            offset += sizeof(value);

            cs[symbol] = value;
        }
    }

    ::close(fd);
}

}  // namespace quant::storage
