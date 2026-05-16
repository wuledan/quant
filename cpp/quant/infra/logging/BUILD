# BUILD - C++ logging and metrics library

package(default_visibility = ["//visibility:public"])


cc_library(
    name = "logging",
    srcs = [
        "logger.cc",
        "json_file_sink.cc",
        "console_sink.cc",
        "metrics.cc",
    ],
    hdrs = [
        "log_sink.h",
        "logger.h",
        "json_file_sink.h",
        "console_sink.h",
        "metrics.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        "//cpp/quant/infra:infra",
    ],
)
