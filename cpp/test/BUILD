# BUILD - C++ test targets

package(default_visibility = ["//visibility:public"])


cc_test(
    name = "error_codes_test",
    srcs = ["error_codes_test.cc"],
    deps = [
        "//cpp/quant/infra:infra",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "thread_pool_test",
    srcs = ["thread_pool_test.cc"],
    deps = [
        "//cpp/quant/infra:infra",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "memory_pool_test",
    srcs = ["memory_pool_test.cc"],
    deps = [
        "//cpp/quant/infra:infra",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "object_pool_test",
    srcs = ["object_pool_test.cc"],
    deps = [
        "//cpp/quant/infra:infra",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "time_utils_test",
    srcs = ["time_utils_test.cc"],
    deps = [
        "//cpp/quant/infra:infra",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

# ── New test targets ──

cc_test(
    name = "event_types_test",
    srcs = ["event_types_test.cc"],
    deps = [
        "//cpp/quant/event/events:event_types",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "event_bus_test",
    srcs = ["event_bus_test.cc"],
    deps = [
        "//cpp/quant/event:event",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "logger_test",
    srcs = ["logger_test.cc"],
    deps = [
        "//cpp/quant/infra/logging:logging",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "metrics_test",
    srcs = ["metrics_test.cc"],
    deps = [
        "//cpp/quant/infra/logging:logging",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "config_test",
    srcs = ["config_test.cc"],
    deps = [
        "//cpp/quant/infra/config:config",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "execution_test",
    srcs = ["execution_test.cc"],
    deps = [
        "//cpp/quant/execution:execution",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "risk_test",
    srcs = ["risk_test.cc"],
    deps = [
        "//cpp/quant/risk:risk",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)

cc_test(
    name = "network_test",
    srcs = ["network_test.cc"],
    deps = [
        "//cpp/quant/network:network",
        "@com_google_googletest//:gtest_main",
    ],
    copts = ["-std=c++20"],
)