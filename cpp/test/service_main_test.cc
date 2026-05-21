// service_main_test.cc — Verify the C++ resident service can initialize and shut down cleanly
//
// This test does NOT start network I/O. It only verifies that:
// 1. GlobalExecutor can be initialized
// 2. StorageEngine can be created with default options
// 3. EventBus can be created and started
// 4. StrategyEngine can be created
// 5. SchedulerService can be created and started
// 6. All components can be shut down cleanly

#include <gtest/gtest.h>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/network/global_executor.h"
#include "cpp/quant/scheduler/scheduler_service.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/strategy/strategy_engine.h"

using namespace quant;

// ── Test: GlobalExecutor initialization ──
TEST(ServiceMainTest, GlobalExecutorInit) {
    auto& exec = network::GlobalExecutor::instance();
    exec.init();
    // GlobalExecutor is a singleton — calling init() multiple times is safe
    exec.init();
    SUCCEED();
}

// ── Test: StorageEngine creation with default options ──
TEST(ServiceMainTest, StorageEngineCreation) {
    storage::StorageEngine::Options opts;
    opts.data_dir = "/tmp/quant_test_service";
    opts.cache_budget_mb = 64;
    storage::StorageEngine engine(opts);
    EXPECT_NO_THROW(engine.close());
}

// ── Test: EventBus start/stop ──
TEST(ServiceMainTest, EventBusStartStop) {
    event::EventBus::Options opts;
    event::EventBus bus(opts);
    bus.start();
    bus.stop();
    SUCCEED();
}

// ── Test: StrategyEngine creation ──
TEST(ServiceMainTest, StrategyEngineCreation) {
    storage::StorageEngine::Options storage_opts;
    storage_opts.data_dir = "/tmp/quant_test_service";
    storage::StorageEngine storage(storage_opts);

    event::EventBus::Options bus_opts;
    event::EventBus bus(bus_opts);
    bus.start();

    strategy::StrategyEngine engine(bus, storage);
    // No strategies activated yet — is_active(0) should be false
    EXPECT_FALSE(engine.is_active(0));

    bus.stop();
    storage.close();
}

// ── Test: SchedulerService start/stop ──
TEST(ServiceMainTest, SchedulerServiceStartStop) {
    scheduler::SchedulerService service;
    service.start();
    service.stop();
    SUCCEED();
}

// ── Test: Full service lifecycle (no network I/O) ──
TEST(ServiceMainTest, FullLifecycleNoNetwork) {
    // 1. GlobalExecutor
    auto& global_exec = network::GlobalExecutor::instance();
    global_exec.init();

    // 2. StorageEngine
    storage::StorageEngine::Options storage_opts;
    storage_opts.data_dir = "/tmp/quant_test_service";
    storage_opts.cache_budget_mb = 64;
    storage::StorageEngine storage(storage_opts);

    // 3. EventBus
    event::EventBus::Options bus_opts;
    event::EventBus bus(bus_opts);
    bus.start();

    // 4. StrategyEngine
    strategy::StrategyEngine strategy_engine(bus, storage);

    // 5. SchedulerService
    scheduler::SchedulerService scheduler_service;
    scheduler_service.start();

    // ── Simulate running for a brief moment ──
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── Graceful shutdown ──
    scheduler_service.stop();
    bus.stop();
    storage.close();
    global_exec.shutdown();

    SUCCEED();
}
