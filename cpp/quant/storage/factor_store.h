// factor_store.h — Cross-sectional factor storage
//
// Stores factor values per (date, symbol, factor_name).
// In-memory: unordered_map<factor_name, map<date, unordered_map<symbol, value>>>
// Disk: binary .fctr files per (factor_name, year) in data_dir/factors/
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cpp/quant/infra/coroutine.h"

namespace quant::storage {

using infra::CoTask;

// ── Factor file constants ──
constexpr uint32_t kFactorMagic = 0x46435452;  // "FCTR"
constexpr uint32_t kFactorVersion = 1;
constexpr size_t   kFactorHeaderSize = 64;

#pragma pack(push, 1)
struct FactorFileHeader {
    uint32_t magic = kFactorMagic;
    uint32_t version = kFactorVersion;
    uint32_t num_dates;
    uint8_t  reserved[52];  // padding to 64 bytes
};
static_assert(sizeof(FactorFileHeader) == kFactorHeaderSize);
#pragma pack(pop)

// ── FactorStore ──
class FactorStore {
public:
    struct Options {
        std::filesystem::path data_dir;
        size_t cache_capacity = 10000;  // max (date, symbol) entries in memory
    };

    explicit FactorStore(Options opts);

    // ── Write API ──
    void put_factor(int64_t date, const std::string& symbol,
                    const std::string& factor_name, double value);

    void put_factor_batch(int64_t date, const std::string& factor_name,
                          const std::vector<std::pair<std::string, double>>& values);

    // ── Read API ──
    std::vector<double> query_factor(const std::string& symbol,
                                      const std::string& factor_name,
                                      int64_t start_date, int64_t end_date) const;

    std::vector<std::pair<std::string, double>> query_cross_section(
        int64_t date, const std::string& factor_name) const;

    // ── Persistence ──
    void flush() const;
    void load();

    // ── Stats ──
    size_t num_factors() const noexcept { return data_.size(); }
    size_t num_entries() const noexcept;

private:
    // factor_name → date → (symbol → value)
    using CrossSection = std::unordered_map<std::string, double>;
    using DateMap = std::map<int64_t, CrossSection>;
    std::unordered_map<std::string, DateMap> data_;

    Options opts_;

    // Build the file path for a (factor_name, year)
    std::filesystem::path factor_file_path(const std::string& factor_name,
                                            int year) const;

    // Write a single (factor_name, year) group to its .fctr file
    void flush_year(const std::string& factor_name, int year,
                    const DateMap& dates) const;

    // Read a single .fctr file and merge into data_
    void load_file(const std::filesystem::path& path);
};

}  // namespace quant::storage
