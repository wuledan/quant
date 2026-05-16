# BUILD - C++ infrastructure library

package(default_visibility = ["//visibility:public"])


cc_library(
    name = "infra",
    srcs = [
        "error_codes.cc",
        "memory_pool.cc",
        "thread_pool.cc",
        "time_utils.cc",
    ],
    hdrs = [
        "error_codes.h",
        "memory_pool.h",
        "object_pool.h",  # header-only
        "thread_pool.h",
        "time_utils.h",
    ],
    copts = ["-std=c++20"],
    include_prefix = "quant/infra",
    strip_include_prefix = ".",
)

cc_library(
    name = "infra_headers",
    hdrs = [
        "error_codes.h",
        "memory_pool.h",
        "object_pool.h",
        "thread_pool.h",
        "time_utils.h",
    ],
    copts = ["-std=c++20"],
    include_prefix = "quant/infra",
    strip_include_prefix = ".",
)
