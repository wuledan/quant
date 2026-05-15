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

# ── Tests ──
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.16.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

# Error codes test
add_executable(error_codes_test cpp/test/error_codes_test.cc)
target_link_libraries(error_codes_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME error_codes_test COMMAND error_codes_test)

# Thread pool test
add_executable(thread_pool_test cpp/test/thread_pool_test.cc)
target_link_libraries(thread_pool_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME thread_pool_test COMMAND thread_pool_test)

# Memory pool test
add_executable(memory_pool_test cpp/test/memory_pool_test.cc)
target_link_libraries(memory_pool_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME memory_pool_test COMMAND memory_pool_test)

# Object pool test
add_executable(object_pool_test cpp/test/object_pool_test.cc)
target_link_libraries(object_pool_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME object_pool_test COMMAND object_pool_test)

# Time utils test
add_executable(time_utils_test cpp/test/time_utils_test.cc)
target_link_libraries(time_utils_test PRIVATE quant_infra GTest::gtest_main)
add_test(NAME time_utils_test COMMAND time_utils_test)