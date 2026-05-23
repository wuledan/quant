// service_main.cc — C++ resident service process entry point
//
// Starts all C++ components as a resident process:
// 1. GlobalExecutor (WorkStealingExecutor singleton)
// 2. StorageEngine with default options
// 3. EventBus and start it
// 4. StrategyEngine (with EventBus + StorageEngine)
// 5. SchedulerService and start it
// 6. DataIngestor with a default DataSourceConfig
// 7. HttpServer for REST API (StrategyApi routing)
// 8. WebSocketServer for frontend communication
// 9. Handle graceful shutdown on SIGINT/SIGTERM

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <folly/init/Init.h>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/api/strategy_api.h"
#include "cpp/quant/backtest/backtest_runner.h"
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/infra/etcd_client.h"
#include "cpp/quant/infra/strategy_watcher.h"
#include "cpp/quant/ingest/data_ingestor.h"
#include "cpp/quant/ingest/tushare_ingestor.h"
#include "cpp/quant/network/global_executor.h"
#include "cpp/quant/network/http_server.h"
#include "cpp/quant/network/websocket_server.h"
#include "cpp/quant/network/ws_event_bridge.h"
#include "cpp/quant/scheduler/scheduler_service.h"
#include "cpp/quant/storage/cold_upload_daemon.h"
#include "cpp/quant/storage/data_initializer.h"
#include "cpp/quant/storage/remote_storage.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/storage/write_ahead_log.h"
#include "cpp/quant/storage/write_buffer.h"
#include "cpp/quant/strategy/strategy_engine.h"

using namespace quant;

// ── Global state for signal handler ──
static volatile std::sig_atomic_t g_shutdown_requested = 0;
static infra::AffinityBaton g_shutdown_baton;

static void signal_handler(int sig) {
    g_shutdown_requested = 1;
    g_shutdown_baton.post_direct();
    std::cout << "\n[Service] Received signal " << sig << ", shutting down...\n";
}

int main(int argc, char* argv[]) {
    // Initialize folly (required for Timekeeper/Singleton infra before any
    // coroutine library component is accessed).
    folly::init(&argc, &argv);

    std::cout << "[Service] QuantInvest C++ resident service starting...\n";

    // ── Register signal handlers ──
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── 1. Initialize GlobalExecutor ──
    auto& global_exec = network::GlobalExecutor::instance();
    global_exec.init(32);
    std::cout << "[Service] GlobalExecutor initialized (32 workers)\n";

    // ── 2. Create StorageEngine ──
    storage::StorageEngine::Options storage_opts;
    storage_opts.data_dir = "./data";
    storage_opts.cache_budget_mb = 256;
    storage::StorageEngine storage(storage_opts);
    storage.start();
    std::cout << "[Service] StorageEngine created and started (data_dir=./data, cache_budget=256MB)\n";

    // ── 2.5 Create WriteBuffer with WAL for buffered writes ──
    storage::WriteBuffer::Options wb_opts;
    wb_opts.wal_opts = storage::WriteAheadLog::Options{"./data/wal", 64};
    wb_opts.flush_row_threshold = 8192;        // flush after 8K rows
    wb_opts.flush_interval = std::chrono::milliseconds(5000);  // or every 5 seconds
    wb_opts.enable_wal = true;
    auto write_buffer = std::make_unique<storage::WriteBuffer>(storage, wb_opts);
    std::cout << "[Service] WriteBuffer created (WAL=./data/wal, flush_threshold=8192, flush_interval=5s)\n";

    // Recover any uncommitted data from WAL (crash recovery)
    write_buffer->recover();
    std::cout << "[Service] WAL recovery completed\n";

    // Start background periodic flush on the executor
    write_buffer->start_background_flush(global_exec.executor());
    std::cout << "[Service] Background flush started\n";

    // Attach WriteBuffer to StorageEngine
    storage.set_write_buffer(std::move(write_buffer));
    std::cout << "[Service] WriteBuffer attached to StorageEngine\n";

    // ── Load historical data from CSV ──
    storage::DataInitializer data_init(storage);
    int loaded = data_init.load_csv_dir("./data/csv_daily");
    auto init_stats = data_init.stats();
    if (loaded > 0) {
        std::cout << "[Service] Loaded " << init_stats.rows_loaded << " rows from "
                  << init_stats.files_loaded << " files (" << init_stats.rows_failed << " failed)\n";
        // Flush all loaded data to disk segments
        storage.flush_all();
        std::cout << "[Service] Flushed all loaded data to disk segments\n";
    } else {
        std::cout << "[Service] No historical data found in ./data/csv_daily\n";
    }

    // ── 3. Create EventBus and start it ──
    event::EventBus::Options bus_opts;
    event::EventBus bus(bus_opts);
    bus.start();
    std::cout << "[Service] EventBus started\n";

    // ── 4. Create StrategyEngine ──
    strategy::StrategyEngine strategy_engine(bus, storage);
    std::cout << "[Service] StrategyEngine created\n";

    // ── 4.5 Create EtcdClient and StrategyWatcher ──
    infra::EtcdClient etcd_client("http://127.0.0.1:2379");
    infra::StrategyWatcher strategy_watcher(etcd_client, strategy_engine);
    if (!etcd_client.is_available()) {
        std::cerr << "[Service] WARNING: etcd is not available at "
                  << "http://127.0.0.1:2379 — strategy/backtest config will not sync\n";
    }
    // StrategyWatcher disabled — etcdctl get_prefix can't handle multi-line
    // IR JSON values. Strategies are registered via REST API instead.
    std::cout << "[Service] StrategyWatcher disabled (use REST API)\n";

    // ── 5. Create SchedulerService and start it ──
    scheduler::SchedulerService scheduler_service;
    scheduler_service.start();
    std::cout << "[Service] SchedulerService started\n";

    // ── 6. Create DataIngestor ──
    ingest::DataSourceConfig ingest_config;
    ingest_config.name = "default";
    ingest_config.host = "localhost";
    ingest_config.port = 9000;
    ingest_config.protocol = "tcp";
    ingest_config.symbols = {"000001.SZ", "600519.SH"};
    ingest_config.reconnect_delay_ms = 5000;
    ingest_config.max_reconnect_attempts = 10;

    ingest::DataIngestor ingestor(storage, bus, ingest_config);
    std::cout << "[Service] DataIngestor created\n";

    // ── 6.2 Create TushareIngestor (if TUSHARE_TOKEN set) ──
    const char* tushare_token = std::getenv("TUSHARE_TOKEN");
    if (tushare_token && tushare_token[0] != '\0') {
        ingest::TushareIngestor::Options tushare_opts;
        tushare_opts.token = tushare_token;
        tushare_opts.symbols = {"000001.SZ", "000002.SZ", "600519.SH",
                                "300750.SZ", "399001.SZ", "399006.SZ"};
        auto tushare_ingestor = std::make_unique<ingest::TushareIngestor>(
            tushare_opts, storage, bus);
        std::move(tushare_ingestor->start()).scheduleOn(global_exec.executor()).start();
        std::cout << "[Service] TushareIngestor started (poll_interval=60s)\n";
    } else {
        std::cout << "[Service] TushareIngestor disabled (set TUSHARE_TOKEN to enable)\n";
    }

    // ── 6.5 Create RemoteStorage and ColdUploadDaemon ──
    storage::RemoteStorage::Options remote_opts;
    remote_opts.endpoint = "http://127.0.0.1:9010";
    remote_opts.access_key = "minioadmin";
    remote_opts.secret_key = "minioadmin";
    remote_opts.bucket_kline = "quant-kline";
    auto remote_storage = std::make_unique<storage::RemoteStorage>(remote_opts);
    // Wire remote storage into TimeSeriesStore for read-through on cache+disk miss
    storage.set_remote_storage(remote_storage.get());

    storage::ColdUploadDaemon cold_daemon(
        storage.disk(), *remote_storage,
        storage::ColdUploadDaemon::Options{
            .scan_interval = std::chrono::hours(1),
            .cold_threshold_days = 30,
            .remove_after_upload = false,
        });
    cold_daemon.run().scheduleOn(global_exec.executor()).start();
    std::cout << "[Service] ColdUploadDaemon started (threshold=30d, scan_interval=1h)\n";

    // ── 7. Create BacktestRunner and StrategyApi ──
    backtest::BacktestRunner backtest_runner(storage, bus);
    api::StrategyApi strategy_api(strategy_engine, backtest_runner, storage);
    std::cout << "[Service] BacktestRunner and StrategyApi created\n";

    // ── 8. Create HttpServer for REST API ──
    network::HttpServerConfig http_config;
    http_config.port = 9191;
    http_config.host = "0.0.0.0";

    network::HttpServer http_server(http_config);
    http_server.set_handler([&strategy_api](const network::HttpRequest& req) -> network::HttpResponse {
        auto api_resp = strategy_api.handle_request(req.method, req.path, req.body, req.query_string);
        network::HttpResponse http_resp;
        http_resp.status_code = api_resp.status_code;
        http_resp.body = api_resp.body;
        return http_resp;
    });

    http_server.start();
    std::cout << "[Service] HttpServer started (port=9191)\n";

    // ── 9. Create WebSocketServer ──
    network::WsServerConfig ws_config;
    ws_config.port = 8282;
    ws_config.host = "0.0.0.0";
    ws_config.max_connections = 1000;

    network::WebSocketServer ws_server(ws_config);
    std::cout << "[Service] WebSocketServer created (port=8282)\n";

    // ── Start WebSocketServer ──
    ws_server.start();
    std::cout << "[Service] WebSocketServer started\n";

    // ── 10. Create WsEventBridge: EventBus → WebSocket broadcast ──
    network::WsEventBridge ws_bridge(bus, ws_server);
    ws_bridge.start();
    std::cout << "[Service] WsEventBridge started (EventBus → WebSocket)\n";

    // ── All components initialized ──
    std::cout << "[Service] All components initialized. Service is running.\n";
    std::cout << "[Service] Press Ctrl+C to shut down.\n";

    // ── 8. Wait for shutdown signal ──
    if (!g_shutdown_requested) {
        infra::blockingWait(g_shutdown_baton);
    }

    // ── Graceful shutdown ──
    std::cout << "[Service] Shutting down...\n";

    http_server.stop();
    std::cout << "[Service] HttpServer stopped\n";

    ws_server.stop();
    std::cout << "[Service] WebSocketServer stopped\n";

    // Stop etcd watcher before shutting down strategy_engine dependencies
    strategy_watcher.stop();
    std::cout << "[Service] StrategyWatcher stopped\n";

    scheduler_service.stop();
    std::cout << "[Service] SchedulerService stopped\n";

    bus.stop();
    std::cout << "[Service] EventBus stopped\n";

    storage.close();
    std::cout << "[Service] StorageEngine closed\n";

    global_exec.shutdown();
    std::cout << "[Service] GlobalExecutor shutdown\n";

    std::cout << "[Service] Shutdown complete. Exiting.\n";
    return 0;
}