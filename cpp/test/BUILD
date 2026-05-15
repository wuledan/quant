// BUILD — C++ test targets

package(default_visibility = ["//visibility:public"])

load("@com_google_googletest//:gtest.bzl", "gtest")

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
