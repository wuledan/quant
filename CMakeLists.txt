cmake_minimum_required(VERSION 3.20)

# Policy CMP0099: Allow INTERFACE link targets from build directories
# This is needed because Folly's folly_deps target references build-dir paths
# when using FetchContent for dependencies like fast_float.
cmake_policy(SET CMP0099 NEW)
cmake_policy(SET CMP0150 NEW)

project(quant_invest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Compiler options ──
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

# ── FetchContent (must come before any add_library that needs deps) ──
include(FetchContent)

# ── yaml-cpp ──
FetchContent_Declare(
    yaml-cpp
    URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.7.0.tar.gz
)
FetchContent_MakeAvailable(yaml-cpp)

# ── Folly dependencies ──
option(QUANT_USE_FOLLY "Enable Folly coroutine primitives" ON)
if(QUANT_USE_FOLLY)
    # FastFloat: header-only library required by Folly.
    # Must install headers OUTSIDE the project source/build tree to avoid
    # CMake rejecting the path in folly_deps INTERFACE_INCLUDE_DIRECTORIES.
    # Use /tmp/quant_deps as staging location (persists across rebuilds).
    set(QUANT_DEPS_DIR "${CMAKE_HOME}/.quant_deps")
    if(NOT EXISTS "${QUANT_DEPS_DIR}")
        set(QUANT_DEPS_DIR "/tmp/quant_deps")
    endif()
    set(FASTFLOAT_INSTALL_DIR "${QUANT_DEPS_DIR}/fast_float")
    if(NOT EXISTS "${FASTFLOAT_INSTALL_DIR}/include/fast_float/fast_float.h")
        FetchContent_Declare(
            fast_float
            GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
            GIT_TAG v7.0.0
        )
        FetchContent_MakeAvailable(fast_float)
        file(COPY "${fast_float_SOURCE_DIR}/include/fast_float" DESTINATION "${FASTFLOAT_INSTALL_DIR}/include")
    endif()
    set(FASTFLOAT_INCLUDE_DIR "${FASTFLOAT_INSTALL_DIR}/include" CACHE PATH "FastFloat include dir")

    # Fetch newer liburing headers OUTSIDE the build tree to avoid CMake policy errors.
    # System version (2.5) lacks IORING_OP_RECV_ZC needed by Folly v2025.
    set(LIBURING_HEADERS_DIR "/tmp/quant_deps/liburing")
    if(NOT EXISTS "${LIBURING_HEADERS_DIR}/src/include/liburing.h")
        FetchContent_Declare(
            _liburing_fetch
            GIT_REPOSITORY https://github.com/axboe/liburing.git
            GIT_TAG liburing-2.9
            SOURCE_DIR "${LIBURING_HEADERS_DIR}"
        )
        FetchContent_Populate(_liburing_fetch)
    endif()
    set(LIBURING_INCLUDE_DIR "${LIBURING_HEADERS_DIR}/src/include" CACHE PATH "" FORCE)
    find_library(SYSTEM_LIBURING_LIB NAMES uring liburing PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
    if(SYSTEM_LIBURING_LIB)
        set(LIBURING_LIBRARY "${SYSTEM_LIBURING_LIB}" CACHE FILEPATH "" FORCE)
    endif()

    FetchContent_Declare(
        folly
        GIT_REPOSITORY https://github.com/facebook/folly.git
        GIT_TAG v2025.09.15.00
    )
    set(FOLLY_BUILD_SHARED OFF CACHE BOOL "")
    set(FOLLY_USE_SIMDHASH OFF CACHE BOOL "")
    # Populate Folly source first (without building) so we can patch out
    # io_uring files that require newer kernel features (IORING_OP_RECV_ZC etc).
    FetchContent_GetProperties(folly POPULATED folly_populated)
    if(NOT folly_populated)
        FetchContent_Populate(folly)
        file(REMOVE
            "${folly_SOURCE_DIR}/folly/io/async/IoUring.cpp"
            "${folly_SOURCE_DIR}/folly/io/async/IoUringBackend.cpp"
            "${folly_SOURCE_DIR}/folly/io/async/IoUringEvent.cpp"
            "${folly_SOURCE_DIR}/folly/io/async/IoUringEventBaseLocal.cpp"
            "${folly_SOURCE_DIR}/folly/io/async/IoUringProvidedBufferRing.cpp"
            "${folly_SOURCE_DIR}/folly/io/async/IoUringZeroCopyBufferPool.cpp"
            "${folly_SOURCE_DIR}/folly/io/async/SimpleAsyncIO.cpp"
            "${folly_SOURCE_DIR}/folly/io/async/AsyncIoUringSocket.cpp"
            "${folly_SOURCE_DIR}/folly/io/async/EventBasePoller.cpp"
        )
        add_subdirectory(${folly_SOURCE_DIR} ${folly_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()
endif()

# ── infra library ──
add_library(quant_infra
    cpp/quant/infra/error_codes.cc
    cpp/quant/infra/memory_pool.cc
    cpp/quant/infra/work_stealing_executor.cc
    cpp/quant/infra/time_utils.cc
    cpp/quant/infra/affinity_baton.cc
    cpp/quant/infra/timer_scheduler.cc
)

target_include_directories(quant_infra PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/cpp/quant/infra
)
target_compile_features(quant_infra PUBLIC cxx_std_20)

if(QUANT_USE_FOLLY)
    # FetchContent builds use the target name without namespace (folly, folly_coro),
    # while find_package uses the namespace (Folly::folly, Folly::folly_coro).
    # Since we use FetchContent, reference the un-namespaced targets.
    target_link_libraries(quant_infra PUBLIC folly)
    target_compile_definitions(quant_infra PUBLIC QUANT_USE_FOLLY)
endif()

# ── logging library ──
add_library(quant_logging
    cpp/quant/infra/logging/logger.cc
    cpp/quant/infra/logging/json_file_sink.cc
    cpp/quant/infra/logging/console_sink.cc
    cpp/quant/infra/logging/metrics.cc
)
target_include_directories(quant_logging PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/cpp/quant/infra
    ${CMAKE_SOURCE_DIR}/cpp/quant/infra/logging
)
target_link_libraries(quant_logging PUBLIC quant_infra)

# ── toml11 (header-only) ──
set(TOML11_INCLUDE_DIR /home/wuledan/work/3FS/third_party/toml11)
if(NOT EXISTS ${TOML11_INCLUDE_DIR}/toml.hpp)
    message(FATAL_ERROR "toml11 not found at ${TOML11_INCLUDE_DIR}")
endif()

# ── config library ──
add_library(quant_config
    cpp/quant/infra/config/config_manager.cc
    cpp/quant/infra/config/config_validator.cc
    cpp/quant/infra/config/yaml_source.cc
    cpp/quant/infra/config/toml_source.cc
)
target_include_directories(quant_config PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/cpp/quant/infra
    ${CMAKE_SOURCE_DIR}/cpp/quant/infra/config
    ${TOML11_INCLUDE_DIR}
)
target_link_libraries(quant_config PUBLIC quant_infra yaml-cpp::yaml-cpp)

# ── event library ──
add_library(quant_event
    cpp/quant/event/event_bus.cc
)
target_include_directories(quant_event PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/cpp/quant/infra
)
target_link_libraries(quant_event PUBLIC quant_infra)

# ── storage library ──
add_library(quant_storage
    cpp/quant/storage/column_block.cc
    cpp/quant/storage/time_series_cache.cc
    cpp/quant/storage/time_series_store.cc
    cpp/quant/storage/disk_persistence.cc
    cpp/quant/storage/storage_engine.cc
)
target_include_directories(quant_storage PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_storage PUBLIC quant_event quant_network)

# ── execution library ──
add_library(quant_execution
    cpp/quant/execution/order_state_machine.cc
    cpp/quant/execution/order_manager.cc
    cpp/quant/execution/algorithmic_trader.cc
)
target_include_directories(quant_execution PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_execution PUBLIC quant_infra)

# ── scheduler library ──
add_library(quant_scheduler
    cpp/quant/scheduler/task_graph.cc
    cpp/quant/scheduler/wave_scheduler.cc
    cpp/quant/scheduler/cron_scheduler.cc
    cpp/quant/scheduler/scheduler_service.cc
    cpp/quant/scheduler/execution_dag.cc
)
target_include_directories(quant_scheduler PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/cpp/quant/infra
    ${CMAKE_SOURCE_DIR}/cpp/quant/scheduler
)
target_link_libraries(quant_scheduler PUBLIC quant_infra)

# ── factor library ──
add_library(quant_factor
    cpp/quant/factor/factor_registry.cc
    cpp/quant/factor/factor_dag.cc
    cpp/quant/factor/factor_computer.cc
    cpp/quant/factor/built_in_factors.cc
    cpp/quant/factor/op_registry.cc
)
target_include_directories(quant_factor PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_factor PUBLIC quant_infra)

# ── network library ──
add_library(quant_network
    cpp/quant/network/tcp_connection.cc
    cpp/quant/network/websocket_server.cc
    cpp/quant/network/ws_session.cc
    cpp/quant/network/co_io.cc
    cpp/quant/network/global_executor.cc
    cpp/quant/network/global_io.cc
)
target_include_directories(quant_network PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(quant_network PUBLIC ${CMAKE_SOURCE_DIR}/cpp/quant/infra)
target_link_libraries(quant_network PUBLIC quant_infra)

if(QUANT_USE_FOLLY)
    # liburing for io_uring support in CoIouring
    # Use system liburing (2.5, compatible with kernel 6.17)
    find_path(SYSTEM_LIBURING_INCLUDE NAMES liburing.h PATHS /usr/include)
    if(SYSTEM_LIBURING_INCLUDE)
        target_include_directories(quant_network PUBLIC ${SYSTEM_LIBURING_INCLUDE})
    endif()
    find_library(SYSTEM_LIBURING_LIB NAMES uring liburing PATHS /usr/lib/x86_64-linux-gnu)
    if(SYSTEM_LIBURING_LIB)
        target_link_libraries(quant_network PUBLIC ${SYSTEM_LIBURING_LIB})
        target_compile_definitions(quant_network PUBLIC QUANT_USE_LIBURING)
    endif()
endif()

# ── strategy library ──
add_library(quant_strategy
    cpp/quant/strategy/signal_handler.cc
    cpp/quant/strategy/strategy_registry.cc
    cpp/quant/strategy/strategy_engine.cc
)
target_include_directories(quant_strategy PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_strategy PUBLIC quant_execution quant_event quant_factor quant_ir quant_storage)

# ── portfolio library ──
add_library(quant_portfolio
    cpp/quant/portfolio/portfolio.cc
)
target_include_directories(quant_portfolio PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_portfolio PUBLIC quant_execution)

# ── backtest library ──
add_library(quant_backtest
    cpp/quant/backtest/simulated_broker.cc
    cpp/quant/backtest/backtest_runner.cc
)
target_include_directories(quant_backtest PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_backtest PUBLIC quant_portfolio quant_strategy quant_factor quant_ir quant_storage quant_event quant_infra)

# ── risk library ──
add_library(quant_risk
    cpp/quant/risk/risk_engine.cc
    cpp/quant/risk/risk_rule.cc
    cpp/quant/risk/risk_alert_publisher.cc
)
target_include_directories(quant_risk PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_risk PUBLIC quant_event)

# ── ingest library ──
add_library(quant_ingest
    cpp/quant/ingest/data_ingestor.cc
)
target_include_directories(quant_ingest PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_ingest PUBLIC quant_storage quant_event quant_network)

# ── IR library ──
add_library(quant_ir
    cpp/quant/ir/ir_graph.cc
)
target_include_directories(quant_ir PUBLIC ${CMAKE_SOURCE_DIR})

# ── pybind11 module (optional, requires pybind11 installed) ──
find_package(pybind11 CONFIG QUIET)
if(pybind11_FOUND)
    pybind11_add_module(_quant_core
        cpp/quant/pybind/pybind_module.cc
        cpp/quant/pybind/py_storage.cc
        cpp/quant/pybind/py_factor.cc
        cpp/quant/pybind/py_execution.cc
        cpp/quant/pybind/py_risk.cc
        cpp/quant/pybind/py_ingest.cc
    )
    target_include_directories(_quant_core PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(_quant_core PRIVATE
        quant_storage
        quant_factor
        quant_execution
        quant_risk
        quant_ingest
        quant_event
        quant_infra
    )
    set_target_properties(_quant_core PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/py/src/quant_invest"
    )
    message(STATUS "pybind11 found — _quant_core module will be built")
else()
    message(STATUS "pybind11 not found — skipping _quant_core module")
endif()

# ── Tests ──
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.16.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

# Existing tests
add_executable(error_codes_test cpp/test/error_codes_test.cc)
target_link_libraries(error_codes_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME error_codes_test COMMAND error_codes_test)

add_executable(work_stealing_executor_test cpp/test/work_stealing_executor_test.cc)
target_link_libraries(work_stealing_executor_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME work_stealing_executor_test COMMAND work_stealing_executor_test)

add_executable(memory_pool_test cpp/test/memory_pool_test.cc)
target_link_libraries(memory_pool_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME memory_pool_test COMMAND memory_pool_test)

add_executable(object_pool_test cpp/test/object_pool_test.cc)
target_link_libraries(object_pool_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME object_pool_test COMMAND object_pool_test)

add_executable(time_utils_test cpp/test/time_utils_test.cc)
target_link_libraries(time_utils_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME time_utils_test COMMAND time_utils_test)

# ── New test targets ──

add_executable(event_types_test cpp/test/event_types_test.cc)
target_link_libraries(event_types_test PRIVATE quant_event GTest::gtest_main)
add_test(NAME event_types_test COMMAND event_types_test)

add_executable(event_bus_test cpp/test/event_bus_test.cc)
target_link_libraries(event_bus_test PRIVATE quant_event GTest::gtest_main)
add_test(NAME event_bus_test COMMAND event_bus_test)

add_executable(logger_test cpp/test/logger_test.cc)
target_link_libraries(logger_test PRIVATE quant_logging GTest::gtest_main)
add_test(NAME logger_test COMMAND logger_test)

add_executable(metrics_test cpp/test/metrics_test.cc)
target_link_libraries(metrics_test PRIVATE quant_logging GTest::gtest_main)
add_test(NAME metrics_test COMMAND metrics_test)

add_executable(config_test cpp/test/config_test.cc)
target_link_libraries(config_test PRIVATE quant_config GTest::gtest_main)
add_test(NAME config_test COMMAND config_test)

add_executable(storage_test cpp/test/storage_test.cc)
target_link_libraries(storage_test PRIVATE quant_storage GTest::gtest_main)
add_test(NAME storage_test COMMAND storage_test)

add_executable(factor_test cpp/test/factor_test.cc)
target_link_libraries(factor_test PRIVATE quant_factor GTest::gtest_main)
add_test(NAME factor_test COMMAND factor_test)

add_executable(execution_test cpp/test/execution_test.cc)
target_link_libraries(execution_test PRIVATE quant_execution GTest::gtest_main)
add_test(NAME execution_test COMMAND execution_test)

add_executable(risk_test cpp/test/risk_test.cc)
target_link_libraries(risk_test PRIVATE quant_risk GTest::gtest_main)
add_test(NAME risk_test COMMAND risk_test)

add_executable(data_ingestor_test cpp/quant/ingest/test_data_ingestor.cc)
target_link_libraries(data_ingestor_test PRIVATE quant_ingest GTest::gtest_main)
add_test(NAME data_ingestor_test COMMAND data_ingestor_test)

add_executable(network_test cpp/test/network_test.cc)
target_link_libraries(network_test PRIVATE quant_network GTest::gtest_main)
add_test(NAME network_test COMMAND network_test)

add_executable(scheduler_test cpp/test/scheduler_test.cc)
target_link_libraries(scheduler_test PRIVATE quant_scheduler GTest::gtest_main)
add_test(NAME scheduler_test COMMAND scheduler_test)

add_executable(execution_dag_test cpp/test/execution_dag_test.cc)
target_link_libraries(execution_dag_test PRIVATE quant_scheduler quant_infra folly GTest::gtest_main)
add_test(NAME execution_dag_test COMMAND execution_dag_test)

# ── AffinityBaton test ──
add_executable(affinity_baton_test cpp/test/affinity_baton_test.cc)
target_link_libraries(affinity_baton_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME affinity_baton_test COMMAND affinity_baton_test)

# ── Coroutine and thread-local stats tests ──
add_executable(coroutine_test cpp/test/coroutine_test.cc)
target_link_libraries(coroutine_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME coroutine_test COMMAND coroutine_test)

add_executable(thread_local_stats_test cpp/test/thread_local_stats_test.cc)
target_link_libraries(thread_local_stats_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME thread_local_stats_test COMMAND thread_local_stats_test)

add_executable(chase_lev_deque_test cpp/test/chase_lev_deque_test.cc)
target_link_libraries(chase_lev_deque_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME chase_lev_deque_test COMMAND chase_lev_deque_test)

# ── TimerScheduler test ──
if(QUANT_USE_FOLLY)
    add_executable(timer_scheduler_test cpp/test/timer_scheduler_test.cc)
    target_link_libraries(timer_scheduler_test PRIVATE quant_infra folly GTest::gtest_main)
    add_test(NAME timer_scheduler_test COMMAND timer_scheduler_test)
endif()

# ── Folly smoke test ──
if(QUANT_USE_FOLLY)
    add_executable(folly_smoke_test cpp/test/folly_smoke_test.cc)
    target_link_libraries(folly_smoke_test PRIVATE quant_infra folly GTest::gtest_main)
    add_test(NAME folly_smoke_test COMMAND folly_smoke_test)
endif()

# ── Coroutine-aware component tests ──
if(QUANT_USE_FOLLY)
    add_executable(wave_scheduler_co_test cpp/test/wave_scheduler_co_test.cc)
    target_include_directories(wave_scheduler_co_test PRIVATE ${CMAKE_SOURCE_DIR}/cpp/quant/infra)
    target_link_libraries(wave_scheduler_co_test PRIVATE quant_scheduler quant_infra folly GTest::gtest_main)
    add_test(NAME wave_scheduler_co_test COMMAND wave_scheduler_co_test)
endif()

if(QUANT_USE_FOLLY)
    add_executable(cron_scheduler_co_test cpp/test/cron_scheduler_co_test.cc)
    target_include_directories(cron_scheduler_co_test PRIVATE ${CMAKE_SOURCE_DIR}/cpp/quant/infra)
    target_link_libraries(cron_scheduler_co_test PRIVATE quant_scheduler quant_infra folly GTest::gtest_main)
    add_test(NAME cron_scheduler_co_test COMMAND cron_scheduler_co_test)
endif()

if(QUANT_USE_FOLLY)
    add_executable(event_bus_co_test cpp/test/event_bus_co_test.cc)
    target_include_directories(event_bus_co_test PRIVATE ${CMAKE_SOURCE_DIR}/cpp/quant/infra)
    target_link_libraries(event_bus_co_test PRIVATE quant_event quant_infra folly GTest::gtest_main)
    add_test(NAME event_bus_co_test COMMAND event_bus_co_test)
endif()

if(QUANT_USE_FOLLY)
    add_executable(logger_co_test cpp/test/logger_co_test.cc)
    target_include_directories(logger_co_test PRIVATE ${CMAKE_SOURCE_DIR}/cpp/quant/infra)
    target_link_libraries(logger_co_test PRIVATE quant_logging quant_infra folly GTest::gtest_main)
    add_test(NAME logger_co_test COMMAND logger_co_test)
endif()

if(QUANT_USE_FOLLY)
    add_executable(config_co_test cpp/test/config_co_test.cc)
    target_include_directories(config_co_test PRIVATE ${CMAKE_SOURCE_DIR}/cpp/quant/infra)
    target_link_libraries(config_co_test PRIVATE quant_config quant_infra folly GTest::gtest_main)
    add_test(NAME config_co_test COMMAND config_co_test)
endif()

# ── Engine benchmark ──
if(QUANT_USE_FOLLY)
    add_executable(engine_benchmark cpp/test/engine_benchmark.cc)
    target_include_directories(engine_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/cpp/quant/infra)
    target_link_libraries(engine_benchmark PRIVATE quant_scheduler quant_infra folly GTest::gtest_main)
    add_test(NAME engine_benchmark COMMAND engine_benchmark)
endif()

# ── CoIouring (io_uring coroutine I/O) test ──

if(QUANT_USE_FOLLY)
    add_executable(integration_test cpp/test/integration_test.cc)
    target_include_directories(integration_test PRIVATE ${CMAKE_SOURCE_DIR}/cpp/quant/infra ${CMAKE_SOURCE_DIR}/cpp/quant/scheduler)
    target_link_libraries(integration_test PRIVATE quant_scheduler quant_infra folly GTest::gtest_main)
    add_test(NAME integration_test COMMAND integration_test)
endif()

if(QUANT_USE_FOLLY)
    add_executable(co_io_test cpp/test/co_io_test.cc)
    target_include_directories(co_io_test PRIVATE ${CMAKE_SOURCE_DIR}/cpp/quant/infra)
    target_link_libraries(co_io_test PRIVATE quant_network quant_infra folly GTest::gtest_main)
    if(DEFINED SYSTEM_LIBURING_INCLUDE)
        target_include_directories(co_io_test PRIVATE ${SYSTEM_LIBURING_INCLUDE})
    endif()
    if(DEFINED SYSTEM_LIBURING_LIB)
        target_link_libraries(co_io_test PRIVATE ${SYSTEM_LIBURING_LIB})
    endif()
    add_test(NAME co_io_test COMMAND co_io_test)
endif()

# ── IR graph test ──
add_executable(ir_graph_test cpp/test/ir_graph_test.cc)
target_link_libraries(ir_graph_test PRIVATE quant_ir GTest::gtest_main)
add_test(NAME ir_graph_test COMMAND ir_graph_test)

# ── OpRegistry test ──
add_executable(op_registry_test cpp/test/op_registry_test.cc)
target_link_libraries(op_registry_test PRIVATE quant_factor GTest::gtest_main)
add_test(NAME op_registry_test COMMAND op_registry_test)

# ── FactorDAG from_graph test ──
add_executable(factor_dag_from_graph_test cpp/test/factor_dag_from_graph_test.cc)
target_link_libraries(factor_dag_from_graph_test PRIVATE quant_factor quant_ir GTest::gtest_main)
add_test(NAME factor_dag_from_graph_test COMMAND factor_dag_from_graph_test)

# ── Signal handler test ──
add_executable(signal_handler_test cpp/test/signal_handler_test.cc)
target_link_libraries(signal_handler_test PRIVATE quant_strategy GTest::gtest_main)
add_test(NAME signal_handler_test COMMAND signal_handler_test)

# ── Backtest test ──
add_executable(backtest_test cpp/test/backtest_test.cc)
target_link_libraries(backtest_test PRIVATE quant_backtest GTest::gtest_main)
add_test(NAME backtest_test COMMAND backtest_test)

# ── Strategy center test ──
add_executable(strategy_center_test cpp/test/strategy_center_test.cc)
target_link_libraries(strategy_center_test PRIVATE quant_strategy GTest::gtest_main)
add_test(NAME strategy_center_test COMMAND strategy_center_test)