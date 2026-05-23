// data_initializer.h — Bulk CSV loader for StorageEngine
//
// Reads historical kline data from CSV files and writes to StorageEngine
// via store_kline_batch(). Used at service startup to populate the store.
#pragma once

#include <atomic>
#include <string>

#include "cpp/quant/storage/column_block.h"  // for KlineFreq

namespace quant::event { struct KlineRow; }
namespace quant::storage { class StorageEngine; }

namespace quant::storage {

// Infer K-line frequency from filename based on substrings:
// "1min"/"5min"/"15min"/"30min"/"60min" → corresponding KlineFreq
// "day" or no match → KlineFreq::kDay (default)
KlineFreq infer_freq_from_filename(const std::string& filename);

struct LoadStats {
    int files_loaded{0};
    int rows_loaded{0};
    int rows_failed{0};
};

class DataInitializer {
public:
    explicit DataInitializer(StorageEngine& engine);

    // Load a single CSV file (may contain one or more symbols)
    // CSV header: symbol,date,open,high,low,close,volume,amount
    // Prices are float (×10000 conversion happens internally)
    // freq: Kline frequency for the loaded data (default: daily)
    bool load_csv(const std::string& csv_path,
                  KlineFreq freq = KlineFreq::kDay);

    // Load all .csv files from a directory
    // Returns number of files successfully loaded
    int load_csv_dir(const std::string& dir_path);

    LoadStats stats() const noexcept;

private:
    bool parse_csv_row(const std::string& line, std::string& symbol,
                       int64_t& ts, event::KlineRow& row);

    StorageEngine& engine_;
    std::atomic<int> files_loaded_{0};
    std::atomic<int> rows_loaded_{0};
    std::atomic<int> rows_failed_{0};
};

}  // namespace quant::storage
