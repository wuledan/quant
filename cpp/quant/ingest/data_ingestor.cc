// data_ingestor.cc — Coroutine-based market data ingestor implementation
//
// Connects to market data sources, parses kline data, and writes to
// StorageEngine via TimeSeriesStore. Uses coroutines for async I/O.
//
// Network protocol:
//   Length-prefixed JSON messages over TCP.
//   Format: [4-byte length][JSON payload]
//   Price fields arrive as floats from network, converted to int64_t ×10000

#include "cpp/quant/ingest/data_ingestor.h"

#include <cstring>
#include <vector>

#include <arpa/inet.h>  // ntohl

#include <folly/coro/Sleep.h>

#include "cpp/quant/event/events/kline_event.h"
#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/network/global_executor.h"
#include "cpp/quant/network/global_io.h"

namespace quant::ingest {

// ────────────────────────────────────────────────────────────────
// JSON extraction helpers
// ────────────────────────────────────────────────────────────────

// Find a JSON key and return pointer to the start of its value.
// Handles optional whitespace around the colon: "key":value or "key" : value.
static const char* find_json_value(const char* json, size_t len,
                                    const char* key) {
    std::string search = "\"";
    search += key;
    search += "\"";

    const char* p = static_cast<const char*>(
        memmem(json, len, search.data(), search.size()));
    if (!p) return nullptr;

    p += search.size();
    // Skip whitespace before ':'
    while (p < json + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= json + len || *p != ':') return nullptr;
    p++;  // Skip ':'
    // Skip whitespace after ':'
    while (p < json + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= json + len) return nullptr;

    return p;
}

static bool extract_int64(const char* json, size_t len,
                          const char* key, int64_t& out) {
    const char* p = find_json_value(json, len, key);
    if (!p) return false;

    char* end = nullptr;
    out = std::strtoll(p, &end, 10);
    return end != p;
}

static bool extract_string(const char* json, size_t len,
                           const char* key, std::string& out) {
    // Build pattern to find "key":" or with whitespace:
    // Start by finding "key"
    const char* p = find_json_value(json, len, key);
    if (!p) return false;

    // After the colon, expect a quote for string values
    if (*p != '"') return false;
    p++;

    const char* end = static_cast<const char*>(
        memchr(p, '"', static_cast<size_t>((json + len) - p)));
    if (!end) return false;

    out.assign(p, static_cast<size_t>(end - p));
    return true;
}

// Extract a double-precision float from JSON.
// Network protocol sends price fields as floats (e.g. 3400.50).
static bool extract_double(const char* json, size_t len,
                           const char* key, double& out) {
    const char* p = find_json_value(json, len, key);
    if (!p) return false;

    char* end = nullptr;
    out = std::strtod(p, &end);
    return end != p;
}

// Check if a price field value is a float (contains decimal point).
// Network protocol sends prices as floats (e.g. 3400.50), while internal/
// backfill sources may send already-fixed-point integers (e.g. 34005000).
static bool price_is_float(const char* json, size_t len, const char* key) {
    const char* p = find_json_value(json, len, key);
    if (!p) return false;

    // Scan for '.' before ',' or '}' or end
    while (p < json + len && *p != ',' && *p != '}' && *p != '\n') {
        if (*p == '.') return true;
        p++;
    }
    return false;
}

// ────────────────────────────────────────────────────────────────
// Construction / Destruction
// ────────────────────────────────────────────────────────────────

DataIngestor::DataIngestor(
    storage::StorageEngine& engine,
    event::EventBus& bus,
    DataSourceConfig config
) : engine_(engine), bus_(bus), config_(std::move(config)) {}

DataIngestor::~DataIngestor() {
    stop();
}

// ────────────────────────────────────────────────────────────────
// Lifecycle — Coroutine entry point
// ────────────────────────────────────────────────────────────────

CoTask<void> DataIngestor::start() {
    running_.store(true, std::memory_order_release);

    // Run the connect/receive/heartbeat coroutine loop.
    // The caller should schedule this Task on an executor (e.g. via
    // blockingWait or co_withExecutor) for proper thread affinity.
    co_await connect_loop();

    running_.store(false, std::memory_order_release);
}

void DataIngestor::stop() {
    stopping_.store(true, std::memory_order_release);

    // Disconnect the socket so any in-flight co_recv/co_send returns
    // immediately, unblocking the coroutine loops.
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (conn_) {
            conn_->disconnect();
            conn_.reset();
        }
    }

    running_.store(false, std::memory_order_release);
}

// ────────────────────────────────────────────────────────────────
// connect_loop — Reconnection orchestrator
//
// Runs a loop: connect → run receive+heartbeat concurrently →
// detect disconnect → reconnect with backoff.
// ────────────────────────────────────────────────────────────────

CoTask<void> DataIngestor::connect_loop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        // ── 1. Get or create the io_uring instance ──
        auto& io = network::GlobalCoIouring::instance();
        auto* ring = io.io_uring();
        if (!ring) {
            // io_uring not yet initialized; retry after a short delay.
            co_await folly::coro::sleep(std::chrono::milliseconds(500));
            continue;
        }

        // ── 2. Create and configure the TCP connection ──
        network::TcpConfig tcp_config;
        tcp_config.host = config_.host;
        tcp_config.port = config_.port;
        tcp_config.auto_reconnect = false;  // We handle reconnection
        tcp_config.reconnect_delay_ms = config_.reconnect_delay_ms;
        tcp_config.max_reconnect_attempts = 0;
        tcp_config.read_buffer_size = 65536;
        tcp_config.timeout_ms = 30000;

        auto conn = std::make_unique<network::TcpConnection>(
            std::move(tcp_config));
        conn->set_io_uring(ring);

        // ── 3. Connect (async, via io_uring) ──
        bool ok = co_await conn->co_connect();
        if (!ok) {
            reconnect_count_.fetch_add(1, std::memory_order_relaxed);

            if (stopping_.load(std::memory_order_acquire)) break;
            co_await folly::coro::sleep(
                std::chrono::milliseconds(config_.reconnect_delay_ms));
            continue;
        }

        // ── 4. Store the connection (under lock for stop()) ──
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            conn_ = std::move(conn);
        }

        // ── 5. Run receive + heartbeat concurrently ──
        // Both loops access conn_ directly. They only run while this
        // connection is alive — collectAll returns when either loop
        // exits (disconnect or stop), and we reset conn_ afterward.
        co_await folly::coro::collectAll(
            receive_loop(),
            heartbeat_loop()
        );

        // ── 6. Connection lost — clean up ──
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            if (conn_) {
                conn_->disconnect();
                conn_.reset();
            }
        }

        if (stopping_.load(std::memory_order_acquire)) break;

        // Exponential backoff on reconnect
        co_await folly::coro::sleep(
            std::chrono::milliseconds(config_.reconnect_delay_ms));
    }
}

// ────────────────────────────────────────────────────────────────
// receive_loop — Read and process kline messages
//
// Protocol: length-prefixed JSON messages over TCP.
//   [4 bytes: uint32_t payload length (network byte order)]
//   [N bytes: JSON payload]
//
// Each JSON payload is parsed into KlineData and stored/published.
// ────────────────────────────────────────────────────────────────

CoTask<void> DataIngestor::receive_loop() {
    constexpr size_t kHeaderLen = 4;
    constexpr size_t kMaxMsgLen = 1024 * 1024;  // 1 MB max

    while (!stopping_.load(std::memory_order_acquire)) {
        // ── Check connection ──
        network::TcpConnection* conn = nullptr;
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            conn = conn_.get();
        }
        if (!conn || !conn->is_connected()) {
            co_return;
        }

        // ── Read 4-byte length header ──
        uint32_t net_len = 0;
        size_t header_read = 0;
        while (header_read < kHeaderLen) {
            auto n = co_await conn->co_recv(
                reinterpret_cast<char*>(&net_len) + header_read,
                kHeaderLen - header_read);
            if (n <= 0) {
                co_return;  // Connection closed or error
            }
            header_read += static_cast<size_t>(n);
        }

        uint32_t msg_len = ntohl(net_len);
        if (msg_len == 0 || msg_len > kMaxMsgLen) {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // ── Read message body ──
        std::vector<char> buf(msg_len);
        size_t body_read = 0;
        while (body_read < msg_len) {
            auto n = co_await conn->co_recv(
                buf.data() + body_read,
                msg_len - body_read);
            if (n <= 0) {
                co_return;  // Connection closed or error
            }
            body_read += static_cast<size_t>(n);
        }

        bytes_received_.fetch_add(kHeaderLen + msg_len,
                                  std::memory_order_relaxed);

        // ── Parse and process ──
        std::string symbol;
        KlineData kline{};
        if (parse_kline(buf.data(), msg_len, symbol, kline)) {
            klines_received_.fetch_add(1, std::memory_order_relaxed);

            if (store_kline(symbol, kline)) {
                klines_stored_.fetch_add(1, std::memory_order_relaxed);
                publish_kline_event(symbol, kline);
            } else {
                klines_failed_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ────────────────────────────────────────────────────────────────
// heartbeat_loop — Periodic keepalive
//
// Sends a JSON ping message at the configured interval to keep the
// connection alive and detect silent disconnection.
// ────────────────────────────────────────────────────────────────

CoTask<void> DataIngestor::heartbeat_loop() {
    const int interval_ms = config_.heartbeat_interval_ms;
    if (interval_ms <= 0) {
        co_return;  // Heartbeat disabled
    }

    // Pre-allocate the ping message
    std::string ping_msg;
    ping_msg.reserve(64);
    {
        // Build length-prefixed JSON: {"type":"ping"}
        constexpr std::string_view kPingJson = R"({"type":"ping"})";
        uint32_t net_len = htonl(
            static_cast<uint32_t>(kPingJson.size()));
        ping_msg.append(reinterpret_cast<const char*>(&net_len),
                        sizeof(net_len));
        ping_msg.append(kPingJson.data(), kPingJson.size());
    }

    while (!stopping_.load(std::memory_order_acquire)) {
        co_await folly::coro::sleep(
            std::chrono::milliseconds(interval_ms));

        if (stopping_.load(std::memory_order_acquire)) break;

        // Check connection
        network::TcpConnection* conn = nullptr;
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            conn = conn_.get();
        }
        if (!conn || !conn->is_connected()) {
            co_return;  // Signal connect_loop to reconnect
        }

        // Send heartbeat
        auto sent = co_await conn->co_send(
            ping_msg.data(), ping_msg.size());
        if (sent == 0) {
            co_return;  // Send failed, signal disconnect
        }
    }
}

// ────────────────────────────────────────────────────────────────
// Data parsing — JSON kline
// ────────────────────────────────────────────────────────────────

bool DataIngestor::parse_kline(const char* data, size_t len,
                               std::string& symbol, KlineData& kline) {
    if (len < 10) return false;

    if (!extract_string(data, len, "symbol", symbol)) return false;
    if (!extract_int64(data, len, "ts", kline.timestamp)) return false;

    // Price fields arrive as floats from the network protocol (e.g. 3400.50).
    // When the JSON value contains a decimal point, it's a float and needs
    // conversion to int64_t fixed-point (×10000). When the value is an integer,
    // it's already in fixed-point format and should be used as-is.
    double tmp = 0.0;
    if (price_is_float(data, len, "open")) {
        if (!extract_double(data, len, "open", tmp)) return false;
        kline.open = static_cast<int64_t>(tmp * 10000.0 + 0.5);
    } else if (!extract_int64(data, len, "open", kline.open)) {
        return false;
    }

    if (price_is_float(data, len, "high")) {
        if (!extract_double(data, len, "high", tmp)) return false;
        kline.high = static_cast<int64_t>(tmp * 10000.0 + 0.5);
    } else if (!extract_int64(data, len, "high", kline.high)) {
        return false;
    }

    if (price_is_float(data, len, "low")) {
        if (!extract_double(data, len, "low", tmp)) return false;
        kline.low = static_cast<int64_t>(tmp * 10000.0 + 0.5);
    } else if (!extract_int64(data, len, "low", kline.low)) {
        return false;
    }

    if (price_is_float(data, len, "close")) {
        if (!extract_double(data, len, "close", tmp)) return false;
        kline.close = static_cast<int64_t>(tmp * 10000.0 + 0.5);
    } else if (!extract_int64(data, len, "close", kline.close)) {
        return false;
    }

    if (!extract_int64(data, len, "volume", kline.volume)) return false;
    extract_int64(data, len, "amount", kline.amount);

    return true;
}

// ────────────────────────────────────────────────────────────────
// Storage — Write kline to TimeSeriesStore
// ────────────────────────────────────────────────────────────────

bool DataIngestor::store_kline(const std::string& symbol,
                                const KlineData& kline) {
    try {
        constexpr uint8_t dtype = static_cast<uint8_t>(event::DataType::kKlineDay);

        event::KlineRow row;
        row.timestamp = kline.timestamp * 1000;  // ms → μs
        row.open_price = static_cast<int32_t>(kline.open);
        row.high_price = static_cast<int32_t>(kline.high);
        row.low_price = static_cast<int32_t>(kline.low);
        row.close_price = static_cast<int32_t>(kline.close);
        row.volume = kline.volume;
        row.amount = kline.amount;
        row.vwap = 0;

        engine_.store_kline(symbol, dtype, row);
        return true;
    } catch (...) {
        return false;
    }
}

// ────────────────────────────────────────────────────────────────
// Event publishing
// ────────────────────────────────────────────────────────────────

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

// ────────────────────────────────────────────────────────────────
// Manual injection (for testing / backfill)
// ────────────────────────────────────────────────────────────────

bool DataIngestor::ingest_kline(const std::string& symbol,
                                 const KlineData& kline) {
    klines_received_.fetch_add(1, std::memory_order_relaxed);
    if (store_kline(symbol, kline)) {
        klines_stored_.fetch_add(1, std::memory_order_relaxed);
        publish_kline_event(symbol, kline);
        return true;
    }
    klines_failed_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// ────────────────────────────────────────────────────────────────
// Statistics
// ────────────────────────────────────────────────────────────────

IngestorStats DataIngestor::stats() const noexcept {
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        connected = conn_ && conn_->is_connected();
    }

    return IngestorStats{
        .klines_received = klines_received_.load(std::memory_order_relaxed),
        .klines_stored = klines_stored_.load(std::memory_order_relaxed),
        .klines_failed = klines_failed_.load(std::memory_order_relaxed),
        .bytes_received = bytes_received_.load(std::memory_order_relaxed),
        .parse_errors = parse_errors_.load(std::memory_order_relaxed),
        .reconnect_count = reconnect_count_.load(std::memory_order_relaxed),
        .connected = connected,
    };
}

}  // namespace quant::ingest