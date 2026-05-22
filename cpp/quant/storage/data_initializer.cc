// data_initializer.cc — Bulk CSV loader implementation
//
// CSV format (produced by parquet_to_csv.py):
//   symbol,date,open,high,low,close,volume,amount
//   600519.SH,2020-01-02,1130.00,1145.50,1128.00,1140.00,3500000,3980000000
//
// Price fields: float → int32_t (×10000)
// Date: YYYY-MM-DD → int64_t epoch microseconds

#include "cpp/quant/storage/data_initializer.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/storage/storage_engine.h"

namespace quant::storage {

DataInitializer::DataInitializer(StorageEngine& engine)
    : engine_(engine) {}

// ────────────────────────────────────────────────────────────────
// CSV row parser
// ────────────────────────────────────────────────────────────────

bool DataInitializer::parse_csv_row(const std::string& line,
                                     std::string& symbol,
                                     int64_t& ts,
                                     event::KlineRow& row) {
    // Format: symbol,date,open,high,low,close,volume,amount
    std::vector<std::string> fields;
    fields.reserve(8);
    std::string field;
    std::istringstream ss(line);
    while (std::getline(ss, field, ',')) {
        fields.push_back(std::move(field));
    }

    if (fields.size() < 7) return false;

    // symbol
    symbol = fields[0];
    if (symbol.empty()) return false;

    // date → epoch microseconds
    // Format: YYYY-MM-DD
    const auto& date_str = fields[1];
    if (date_str.size() < 10) return false;
    try {
        struct tm tm_val{};
        tm_val.tm_year = std::stoi(date_str.substr(0, 4)) - 1900;
        tm_val.tm_mon = std::stoi(date_str.substr(5, 2)) - 1;
        tm_val.tm_mday = std::stoi(date_str.substr(8, 2));
        tm_val.tm_hour = 15;  // A-share close time
        tm_val.tm_isdst = -1;
        time_t epoch = timegm(&tm_val);
        if (epoch < 0) return false;
        ts = static_cast<int64_t>(epoch) * 1000000;  // microseconds
    } catch (...) {
        return false;
    }

    // Prices: float → int32_t (×10000)
    char* end = nullptr;
    double val = std::strtod(fields[2].c_str(), &end);
    if (end == fields[2].c_str()) return false;
    row.open_price = static_cast<int32_t>(val * 10000.0 + 0.5);

    end = nullptr;
    val = std::strtod(fields[3].c_str(), &end);
    if (end == fields[3].c_str()) return false;
    row.high_price = static_cast<int32_t>(val * 10000.0 + 0.5);

    end = nullptr;
    val = std::strtod(fields[4].c_str(), &end);
    if (end == fields[4].c_str()) return false;
    row.low_price = static_cast<int32_t>(val * 10000.0 + 0.5);

    end = nullptr;
    val = std::strtod(fields[5].c_str(), &end);
    if (end == fields[5].c_str()) return false;
    row.close_price = static_cast<int32_t>(val * 10000.0 + 0.5);

    // Volume
    end = nullptr;
    row.volume = std::strtoll(fields[6].c_str(), &end, 10);
    if (end == fields[6].c_str()) return false;

    // Amount (optional)
    if (fields.size() >= 8 && !fields[7].empty()) {
        end = nullptr;
        row.amount = std::strtoll(fields[7].c_str(), &end, 10);
    } else {
        row.amount = 0;
    }

    row.vwap = 0;

    return true;
}

// ────────────────────────────────────────────────────────────────
// Load single CSV file
// ────────────────────────────────────────────────────────────────

bool DataInitializer::load_csv(const std::string& csv_path,
                               KlineFreq freq) {
    std::ifstream file(csv_path);
    if (!file.is_open()) return false;

    std::string header;
    if (!std::getline(file, header)) return false;  // skip header

    // Group rows by symbol for batch writes
    std::unordered_map<std::string, std::vector<event::KlineRow>> symbol_rows;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::string symbol;
        int64_t ts = 0;
        event::KlineRow row{};
        if (parse_csv_row(line, symbol, ts, row)) {
            row.timestamp = ts;
            symbol_rows[symbol].push_back(row);
            rows_loaded_.fetch_add(1, std::memory_order_relaxed);
        } else {
            rows_failed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Batch write per symbol, using the specified frequency
    for (auto& [sym, rows] : symbol_rows) {
        engine_.store_kline_batch(sym, kline_freq_to_data_type(freq), rows);
    }

    files_loaded_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ────────────────────────────────────────────────────────────────
// Load all CSV files from directory
// ────────────────────────────────────────────────────────────────

int DataInitializer::load_csv_dir(const std::string& dir_path) {
    DIR* dir = ::opendir(dir_path.c_str());
    if (!dir) return 0;

    int count = 0;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name.size() < 5 || name.substr(name.size() - 4) != ".csv") continue;

        std::string path = dir_path;
        if (path.back() != '/') path += '/';
        path += name;

        if (load_csv(path)) {
            count++;
        }
    }

    ::closedir(dir);
    return count;
}

// ────────────────────────────────────────────────────────────────
// Stats
// ────────────────────────────────────────────────────────────────

LoadStats DataInitializer::stats() const noexcept {
    return LoadStats{
        .files_loaded = files_loaded_.load(std::memory_order_relaxed),
        .rows_loaded = rows_loaded_.load(std::memory_order_relaxed),
        .rows_failed = rows_failed_.load(std::memory_order_relaxed),
    };
}

}  // namespace quant::storage
