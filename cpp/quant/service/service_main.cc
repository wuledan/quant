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

#include "cpp/quant/api/strategy_api.h"
#include "cpp/quant/backtest/backtest_runner.h"
#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/ingest/data_ingestor.h"
#include "cpp/quant/network/global_executor.h"
#include "cpp/quant/network/http_server.h"
#include "cpp/quant/network/websocket_server.h"
#include "cpp/quant/scheduler/scheduler_service.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/strategy/strategy_engine.h"

using namespace quant;

// ── Global state for signal handler ──
static volatile std::sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int sig) {
    g_shutdown_requested = 1;
    std::cout << "\n[Service] Received signal " << sig << ", shutting down...\n";
}

int main(int argc, char* argv[]) {
    std::cout << "[Service] QuantInvest C++ resident service starting...\n";

    // ── Register signal handlers ──
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── 1. Initialize GlobalExecutor ──
    auto& global_exec = network::GlobalExecutor::instance();
    global_exec.init();
    std::cout << "[Service] GlobalExecutor initialized\n";

    // ── 2. Create StorageEngine ──
    storage::StorageEngine::Options storage_opts;
    storage_opts.data_dir = "./data";
    storage_opts.cache_budget_mb = 256;
    storage::StorageEngine storage(storage_opts);
    std::cout << "[Service] StorageEngine created (data_dir=./data, cache_budget=256MB)\n";

    // ── 3. Create EventBus and start it ──
    event::EventBus::Options bus_opts;
    event::EventBus bus(bus_opts);
    bus.start();
    std::cout << "[Service] EventBus started\n";

    // ── 4. Create StrategyEngine ──
    strategy::StrategyEngine strategy_engine(bus, storage);
    std::cout << "[Service] StrategyEngine created\n";

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

    ingest::DataIngestor ingestor(storage.store(), bus, ingest_config);
    std::cout << "[Service] DataIngestor created\n";

    // ── 7. Create BacktestRunner and StrategyApi ──
    backtest::BacktestRunner backtest_runner(storage, bus);
    api::StrategyApi strategy_api(strategy_engine, backtest_runner, storage);
    std::cout << "[Service] BacktestRunner and StrategyApi created\n";

    // ── 8. Create HttpServer for REST API ──
    network::HttpServerConfig http_config;
    http_config.port = 9090;
    http_config.host = "0.0.0.0";

    network::HttpServer http_server(http_config);
    http_server.set_handler([&strategy_api](const network::HttpRequest& req) -> network::HttpResponse {
        auto api_resp = strategy_api.handle_request(req.method, req.path, req.body);
        network::HttpResponse http_resp;
        http_resp.status_code = api_resp.status_code;
        http_resp.body = api_resp.body;
        return http_resp;
    });

    http_server.start();
    std::cout << "[Service] HttpServer started (port=9090)\n";

    // ── 9. Create WebSocketServer ──
    network::WsServerConfig ws_config;
    ws_config.port = 8080;
    ws_config.host = "0.0.0.0";
    ws_config.max_connections = 1000;

    network::WebSocketServer ws_server(ws_config);
    std::cout << "[Service] WebSocketServer created (port=8080)\n";

    // ── Start WebSocketServer ──
    ws_server.start();
    std::cout << "[Service] WebSocketServer started\n";

    // ── All components initialized ──
    std::cout << "[Service] All components initialized. Service is running.\n";
    std::cout << "[Service] Press Ctrl+C to shut down.\n";

    // ── 8. Wait for shutdown signal ──
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ── Graceful shutdown ──
    std::cout << "[Service] Shutting down...\n";

    http_server.stop();
    std::cout << "[Service] HttpServer stopped\n";

    ws_server.stop();
    std::cout << "[Service] WebSocketServer stopped\n";

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