// corporate_action_store.h — Corporate actions (dividends, splits, etc.)
//
// All in-memory (<10MB expected). Stores actions per symbol as a sorted
// vector by action_date. Provides backward price adjustment.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace quant::storage {

enum class ActionType : uint8_t {
    kDividend = 0,       // cash dividend
    kSplit = 1,          // stock split
    kRightsIssue = 2,    // rights offering
    kSuspension = 3,     // trading suspension
};

struct CorporateAction {
    std::string symbol;
    int64_t action_date;    // epoch microseconds
    ActionType type;
    double value;            // dividend amount (yuan) or split ratio
    double adjust_factor;    // backward price adjustment factor
};

// ── CorporateActionStore ──
class CorporateActionStore {
public:
    struct Options {
        std::filesystem::path data_dir;
    };

    explicit CorporateActionStore(Options opts);

    void add_action(CorporateAction action);

    // Query actions for a symbol within a date range
    std::vector<CorporateAction> query_actions(const std::string& symbol,
                                                int64_t start_date,
                                                int64_t end_date) const;

    // Backward price adjustment: raw_price at `date` divided by cumulative
    // adjust_factor of all actions after that date.
    double adjust_price(const std::string& symbol, int64_t date,
                        double raw_price) const;

    // ── Persistence (optional, <10MB expected) ──
    void flush() const;
    void load();

    // ── Stats ──
    size_t num_actions() const noexcept;
    size_t num_symbols() const noexcept { return data_.size(); }

private:
    Options opts_;

    // symbol → sorted vector of CorporateAction by action_date
    std::unordered_map<std::string, std::vector<CorporateAction>> data_;
};

}  // namespace quant::storage
