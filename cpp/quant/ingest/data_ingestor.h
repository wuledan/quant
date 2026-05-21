// data_ingestor.h — Coroutine-based market data ingestor
//
// Connects to market data sources (TCP/WebSocket), parses incoming data,
// and writes to StorageEngine via TimeSeriesStore.
//
// Architecture:
//   DataSource → TcpConnection → DataIngestor → TimeSeriesStore
//                                     ↓
//                               EventBus (publish kline/trade events)
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/network/tcp_connection.h"
#include "cpp/quant/storage/time_series_store.h"
#include "cpp/quant/event/event_bus.h"

namespace quant::ingest {

using infra::CoTask;

// ── K-line data structure ──
struct KlineData {
    int64_t timestamp;      // Unix epoch ms
    int64_t open;           // Fixed-point (×10000)
    int64_t high;
    int64_t low;
    int64_t close;
    int64_t volume;
    int64_t amount;
};

// ── Data source configuration ──
struct DataSourceConfig {
    std::string name;           // Source identifier
    std::string host;
    int port = 0;
    std::string protocol = "tcp";  // tcp / ws
    std::vector<std::string> symbols;  // Subscribed symbols
    int reconnect_delay_ms = 1000;
    int max_reconnect_attempts = 10;
    int heartbeat_interval_ms = 30000;
};

// ── Ingestor statistics ──
struct IngestorStats {
    uint64_t klines_received{0};
    uint64_t klines_stored{0};
    uint64_t klines_failed{0};
    uint64_t bytes_received{0};
    uint64_t parse_errors{0};
    uint64_t reconnect_count{0};
    bool connected{false};
};

// ── DataIngestor: coroutine-based market data ingestion ──
class DataIngestor {
public:
    explicit DataIngestor(
        storage::TimeSeriesStore& store,
        event::EventBus& bus,
        DataSourceConfig config
    );
    ~DataIngestor();

    DataIngestor(const DataIngestor&) = delete;
    DataIngestor& operator=(const DataIngestor&) = delete;

    // ── Lifecycle ──
    // Start the ingestor coroutine (connect + receive loop)
    CoTask<void> start();

    // Graceful shutdown
    void stop();

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // ── Statistics ──
    IngestorStats stats() const noexcept;

    // ── Manual data injection (for testing / backfill) ──
    // Directly ingest a kline without network I/O
    bool ingest_kline(const std::string& symbol, const KlineData& kline);

    // ── Data parsing (public for testing) ──
    // Parse a raw JSON message into KlineData
    // Price fields: float → int64_t ×10000, or int → int64_t as-is
    bool parse_kline(const char* data, size_t len,
                     std::string& symbol, KlineData& kline);

private:
    // ── Coroutine tasks ──
    CoTask<void> connect_loop();
    CoTask<void> receive_loop();
    CoTask<void> heartbeat_loop();

    // ── Storage ──
    bool store_kline(const std::string& symbol, const KlineData& kline);

    // ── Event publishing ──
    void publish_kline_event(const std::string& symbol, const KlineData& kline);

    storage::TimeSeriesStore& store_;
    event::EventBus& bus_;
    DataSourceConfig config_;

    std::unique_ptr<network::TcpConnection> conn_;
    mutable std::mutex conn_mutex_;  // Protects conn_ for stop()/coroutine access
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    // Statistics (atomic for lock-free reads)
    std::atomic<uint64_t> klines_received_{0};
    std::atomic<uint64_t> klines_stored_{0};
    std::atomic<uint64_t> klines_failed_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> parse_errors_{0};
    std::atomic<uint64_t> reconnect_count_{0};
};

}  // namespace quant::ingest
