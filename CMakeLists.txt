cmake_minimum_required(VERSION 3.20)
project(quant_invest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Compiler options ──
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

# ── infra library ──
add_library(quant_infra
    cpp/quant/infra/error_codes.cc
    cpp/quant/infra/memory_pool.cc
    cpp/quant/infra/thread_pool.cc
    cpp/quant/infra/time_utils.cc
)

target_include_directories(quant_infra PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_features(quant_infra PUBLIC cxx_std_20)

# ── logging library ──
add_library(quant_logging
    cpp/quant/infra/logging/logger.cc
    cpp/quant/infra/logging/json_file_sink.cc
    cpp/quant/infra/logging/console_sink.cc
    cpp/quant/infra/logging/metrics.cc
)
target_include_directories(quant_logging PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_logging PUBLIC quant_infra)

# ── config library ──
add_library(quant_config
    cpp/quant/infra/config/config_manager.cc
    cpp/quant/infra/config/yaml_source.cc
    cpp/quant/infra/config/toml_source.cc
)
target_include_directories(quant_config PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_config PUBLIC quant_infra)

# ── event library ──
add_library(quant_event
    cpp/quant/event/event_bus.cc
)
target_include_directories(quant_event PUBLIC ${CMAKE_SOURCE_DIR})
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
target_link_libraries(quant_storage PUBLIC quant_event)

# ── execution library ──
add_library(quant_execution
    cpp/quant/execution/order_state_machine.cc
    cpp/quant/execution/order_manager.cc
    cpp/quant/execution/algorithmic_trader.cc
)
target_include_directories(quant_execution PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_execution PUBLIC quant_infra)

# ── risk library ──
add_library(quant_risk
    cpp/quant/risk/risk_engine.cc
)
target_include_directories(quant_risk PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(quant_risk PUBLIC quant_event)

# ── Tests ──
include(FetchContent)
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

add_executable(thread_pool_test cpp/test/thread_pool_test.cc)
target_link_libraries(thread_pool_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME thread_pool_test COMMAND thread_pool_test)

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

add_executable(execution_test cpp/test/execution_test.cc)
target_link_libraries(execution_test PRIVATE quant_execution GTest::gtest_main)
add_test(NAME execution_test COMMAND execution_test)
