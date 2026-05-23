// tushare_ingestor.cc — Tushare HTTP real-time market data ingestor
//
// Polls Tushare daily API via curl subprocess (matching etcd_client/psql
// subprocess pattern). Parses JSON response with minimal overhead and
// feeds KlineRow data into StorageEngine + EventBus.
#include "cpp/quant/ingest/tushare_ingestor.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#include <folly/coro/Sleep.h>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/network/global_executor.h"

namespace quant::ingest {

// ── Constructor / Destructor ──

TushareIngestor::TushareIngestor(Options opts,
                                 storage::StorageEngine& engine,
                                 event::EventBus& bus)
    : opts_(std::move(opts))
    , engine_(engine)
    , bus_(bus) {}

TushareIngestor::~TushareIngestor() {
    stop();
}

// ── HTTP POST via curl subprocess ──

std::string TushareIngestor::http_post(const std::string& body) {
    // Build a temp file for the request body to avoid shell quoting issues
    std::string tmpfile = "/tmp/tushare_req_XXXXXX";
    int fd = mkstemp(tmpfile.data());
    if (fd < 0) return {};

    // Write body
    ssize_t written = write(fd, body.data(), body.size());
    ::close(fd);
    if (written < 0 || static_cast<size_t>(written) != body.size()) {
        unlink(tmpfile.c_str());
        return {};
    }

    std::string cmd = "curl --silent --fail --max-time 10 "
        "-X POST https://api.tushare.pro "
        "-H \"Content-Type: application/json\" "
        "-d @" + tmpfile + " 2>/dev/null";

    std::string result;
    std::array<char, 65536> buffer;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
        }
        pclose(pipe);
    }

    unlink(tmpfile.c_str());
    return result;
}

// ── Date → epoch microseconds ──

int64_t TushareIngestor::date_to_epoch_us(const std::string& date_str) {
    if (date_str.size() < 8) return 0;

    int year = std::stoi(date_str.substr(0, 4));
    int month = std::stoi(date_str.substr(4, 2));
    int day = std::stoi(date_str.substr(6, 2));

    // Days from 1970-01-01 using civil date calculation
    // Formula: days = year*365 + leap_days + day_of_year
    auto is_leap = [](int y) {
        return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    };

    // Days in months for non-leap and leap years
    static const int days_in_months[2][12] = {
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
        {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
    };

    // Count days from 1970 to year-01-01
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) {
        days += is_leap(y) ? 366 : 365;
    }

    // Add days in current year up to (month-1)
    int leap = is_leap(year) ? 1 : 0;
    for (int m = 0; m < month - 1; ++m) {
        days += days_in_months[leap][m];
    }

    // Add days in current month (day - 1, since day 1 = 0 days offset)
    days += (day - 1);

    return days * 86400 * 1000000LL;  // microseconds
}

// ── Price to fixed-point ──

int32_t TushareIngestor::price_to_fixed(double price) {
    return static_cast<int32_t>(llround(price * 10000.0));
}

// ── JSON response parsing ──
//
// Expected format from Tushare daily API:
//   {"data":{"items":[[ts,open,high,low,close,vol,amount],...]}}
//
// Where items inner array has 7 elements:
//   [0] trade_date: string "20260122"
//   [1] open:       number (float)
//   [2] high:       number (float)
//   [3] low:        number (float)
//   [4] close:      number (float)
//   [5] vol:        integer
//   [6] amount:     number (float)
//
// This parser is intentionally minimal — it assumes the Tushare response
// structure and does not validate JSON syntax beyond what's needed.

std::vector<event::KlineRow> TushareIngestor::parse_response(
    const std::string& json, const std::string& symbol) {
    if (json.empty()) return {};

    // Find "items" in the JSON: {"data":{"items":[[...],[...]]}}
    auto items_pos = json.find("\"items\"");
    if (items_pos == std::string::npos) return {};

    // Skip to the colon after "items"
    items_pos = json.find(':', items_pos + 7);
    if (items_pos == std::string::npos) return {};

    // Find the opening bracket of the array
    auto array_start = json.find('[', items_pos + 1);
    if (array_start == std::string::npos) return {};

    // Skip past the outer '['
    size_t pos = array_start + 1;

    std::vector<event::KlineRow> rows;

    while (pos < json.size()) {
        // Skip whitespace
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' ||
               json[pos] == '\t' || json[pos] == '\r')) ++pos;
        if (pos >= json.size()) break;

        // Skip comma separator between inner array items ([...],[...])
        if (json[pos] == ',') {
            ++pos;
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' ||
                   json[pos] == '\t' || json[pos] == '\r')) ++pos;
            if (pos >= json.size()) break;
        }

        // End of outer array
        if (json[pos] == ']') break;

        // Expect inner array start
        if (json[pos] != '[') break;
        ++pos;

        // Parse 7 comma-separated values: [trade_date, open, high, low, close, vol, amount]
        // Field 0: trade_date (quoted string)
        while (pos < json.size() && json[pos] != '"') ++pos;
        if (pos >= json.size()) break;
        ++pos;  // skip opening quote
        auto date_start = pos;
        while (pos < json.size() && json[pos] != '"') ++pos;
        if (pos >= json.size()) break;
        std::string date_str = json.substr(date_start, pos - date_start);
        ++pos;  // skip closing quote

        // Fields 1-6: comma-separated numbers
        double fields[6] = {0};
        for (int i = 0; i < 6; ++i) {
            // Skip comma
            while (pos < json.size() && (json[pos] == ',' || json[pos] == ' ' ||
                   json[pos] == '\n' || json[pos] == '\t' || json[pos] == '\r'))
                ++pos;
            if (pos >= json.size()) break;

            char* endp = nullptr;
            fields[i] = std::strtod(json.c_str() + pos, &endp);
            if (endp == json.c_str() + pos) break;
            pos = static_cast<size_t>(endp - json.c_str());
        }

        // Skip to closing bracket of inner array
        while (pos < json.size() && json[pos] != ']') ++pos;
        if (pos >= json.size()) break;
        ++pos;  // skip ']'

        // Build KlineRow
        event::KlineRow row;
        row.timestamp = date_to_epoch_us(date_str);
        row.open_price = price_to_fixed(fields[0]);   // open
        row.high_price = price_to_fixed(fields[1]);   // high
        row.low_price = price_to_fixed(fields[2]);    // low
        row.close_price = price_to_fixed(fields[3]);  // close
        row.volume = static_cast<int64_t>(fields[4]); // vol
        row.amount = static_cast<int64_t>(fields[5]); // amount
        row.vwap = 0;
        rows.push_back(std::move(row));
    }

    return rows;
}

// ── Today's date in YYYYMMDD ──

static std::string today_yyyymmdd() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    gmtime_r(&tt, &tm_buf);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d%02d%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
    return buf;
}

// ── Poll loop ──

CoTask<void> TushareIngestor::start() {
    while (!stopped_.load(std::memory_order_acquire)) {
        for (const auto& symbol : opts_.symbols) {
            if (stopped_.load(std::memory_order_acquire)) break;
            co_await poll_symbol(symbol);
        }
        if (stopped_.load(std::memory_order_acquire)) break;
        co_await infra::sleep(opts_.poll_interval);
    }
}

void TushareIngestor::stop() {
    stopped_.store(true);
}

CoTask<void> TushareIngestor::poll_loop() {
    // Deprecated — start() runs inline
    co_return;
}

CoTask<void> TushareIngestor::poll_symbol(const std::string& symbol) {
    // Build request body for Tushare daily API
    std::string body = R"({"api_name":"daily","token":")";
    body += opts_.token;
    body += R"(","params":{"ts_code":")";
    body += symbol;
    body += R"(","start_date":")";
    body += today_yyyymmdd();
    body += R"("},"fields":"trade_date,open,high,low,close,vol,amount"})";

    std::string response = http_post(body);
    if (response.empty()) co_return;

    auto rows = parse_response(response, symbol);
    if (rows.empty()) co_return;

    // Store into engine (daily kline → data_type 5)
    constexpr uint8_t kDataTypeDaily = 5;
    engine_.store_kline_batch(symbol, kDataTypeDaily, rows);

    // Publish events
    for (auto& row : rows) {
        auto evt = std::make_unique<event::KlineEvent>();
        evt->symbol = symbol;
        evt->kline_type = event::DataType::kKlineDay;
        evt->kline = row;
        bus_.publish(std::move(evt));
    }
}
}  // namespace quant::ingest
