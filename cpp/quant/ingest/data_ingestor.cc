// data_ingestor.cc — Coroutine-based market data ingestor implementation
//
// Connects to market data sources, parses kline data, and writes to
// StorageEngine via TimeSeriesStore. Uses coroutines for async I/O.

#include "cpp/quant/ingest/data_ingestor.h"

#include <cstring>
#include <vector>

#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/storage/column_block.h"

namespace quant::ingest {

// ── Simple JSON key-value extractor ──
static bool extract_int64(const char* json, size_t len,
                          const char* key, int64_t& out) {
    std::string search = "\"";
    search += key;
    search += "\":";

    const char* p = static_cast<const char*>(
        memmem(json, len, search.data(), search.size()));
    if (!p) return false;

    p += search.size();
    while (p < json + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= json + len) return false;

    char* end = nullptr;
    out = std::strtoll(p, &end, 10);
    return end != p;
}

static bool extract_string(const char* json, size_t len,
                           const char* key, std::string& out) {
    std::string search = "\"";
    search += key;
    search += "\":\"";

    const char* p = static_cast<const char*>(
        memmem(json, len, search.data(), search.size()));
    if (!p) return false;

    p += search.size();
    const char* end = static_cast<const char*>(
        memchr(p, '"', static_cast<size_t>((json + len) - p)));
    if (!end) return false;

    out.assign(p, static_cast<size_t>(end - p));
    return true;
}

// ── Construction / Destruction ──

DataIngestor::DataIngestor(
    storage::TimeSeriesStore& store,
    event::EventBus& bus,
    DataSourceConfig config
) : store_(store), bus_(bus), config_(std::move(config)) {}

DataIngestor::~DataIngestor() {
    stop();
}

// ── Lifecycle ──

CoTask<void> DataIngestor::start() {
    running_.store(true, std::memory_order_release);
    // Network coroutine loop — requires CoIouring integration
    // For now, manual ingestion via ingest_kline() is the primary interface
    co_return;
}

void DataIngestor::stop() {
    stopping_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

// ── Data parsing ──

bool DataIngestor::parse_kline(const char* data, size_t len,
                               std::string& symbol, KlineData& kline) {
    if (len < 10) return false;

    if (!extract_string(data, len, "symbol", symbol)) return false;
    if (!extract_int64(data, len, "ts", kline.timestamp)) return false;
    if (!extract_int64(data, len, "open", kline.open)) return false;
    if (!extract_int64(data, len, "high", kline.high)) return false;
    if (!extract_int64(data, len, "low", kline.low)) return false;
    if (!extract_int64(data, len, "close", kline.close)) return false;
    if (!extract_int64(data, len, "volume", kline.volume)) return false;

    extract_int64(data, len, "amount", kline.amount);

    return true;
}

// ── Storage ──

bool DataIngestor::store_kline(const std::string& symbol, const KlineData& kline) {
    try {
        using namespace quant::storage;
        constexpr uint8_t dtype = static_cast<uint8_t>(event::DataType::kKlineDay);
        const int64_t ts = kline.timestamp;

        // Compress each field as a single-row ColumnBlock
        auto open_block = ColumnBlock::compress(
            DataField::kOpen, std::span<const int64_t>(&kline.open, 1),
            ColumnBlock::Codec::kDelta, ts, ts);
        auto high_block = ColumnBlock::compress(
            DataField::kHigh, std::span<const int64_t>(&kline.high, 1),
            ColumnBlock::Codec::kDelta, ts, ts);
        auto low_block = ColumnBlock::compress(
            DataField::kLow, std::span<const int64_t>(&kline.low, 1),
            ColumnBlock::Codec::kDelta, ts, ts);
        auto close_block = ColumnBlock::compress(
            DataField::kClose, std::span<const int64_t>(&kline.close, 1),
            ColumnBlock::Codec::kDelta, ts, ts);
        auto vol_block = ColumnBlock::compress(
            DataField::kVolume, std::span<const int64_t>(&kline.volume, 1),
            ColumnBlock::Codec::kDelta, ts, ts);

        auto s1 = store_.put(symbol, dtype, std::move(open_block));
        auto s2 = store_.put(symbol, dtype, std::move(high_block));
        auto s3 = store_.put(symbol, dtype, std::move(low_block));
        auto s4 = store_.put(symbol, dtype, std::move(close_block));
        auto s5 = store_.put(symbol, dtype, std::move(vol_block));

        return s1 == StoreStatus::kOk && s2 == StoreStatus::kOk &&
               s3 == StoreStatus::kOk && s4 == StoreStatus::kOk &&
               s5 == StoreStatus::kOk;
    } catch (...) {
        return false;
    }
}

// ── Event publishing ──

void DataIngestor::publish_kline_event(const std::string& symbol,
                                        const KlineData& kline) {
    auto event = std::make_unique<event::KlineEvent>();
    event->symbol = symbol;
    event->kline_type = event::DataType::kKlineDay;
    event->kline.timestamp = kline.timestamp;
    event->kline.open_price = static_cast<int32_t>(kline.open);
    event->kline.high_price = static_cast<int32_t>(kline.high);
    event->kline.low_price = static_cast<int32_t>(kline.low);
    event->kline.close_price = static_cast<int32_t>(kline.close);
    event->kline.volume = kline.volume;
    event->kline.amount = kline.amount;
    event->kline.vwap = 0;

    bus_.publish(std::move(event));
}

// ── Manual injection ──

bool DataIngestor::ingest_kline(const std::string& symbol, const KlineData& kline) {
    klines_received_.fetch_add(1, std::memory_order_relaxed);
    if (store_kline(symbol, kline)) {
        klines_stored_.fetch_add(1, std::memory_order_relaxed);
        publish_kline_event(symbol, kline);
        return true;
    }
    klines_failed_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// ── Statistics ──

IngestorStats DataIngestor::stats() const noexcept {
    return IngestorStats{
        .klines_received = klines_received_.load(std::memory_order_relaxed),
        .klines_stored = klines_stored_.load(std::memory_order_relaxed),
        .klines_failed = klines_failed_.load(std::memory_order_relaxed),
        .bytes_received = bytes_received_.load(std::memory_order_relaxed),
        .parse_errors = parse_errors_.load(std::memory_order_relaxed),
        .reconnect_count = reconnect_count_.load(std::memory_order_relaxed),
        .connected = false,
    };
}

}  // namespace quant::ingest