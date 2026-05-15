package(default_visibility = ["//visibility:public"])

# C++20 compilation flags shared across all targets
COPTS = [
    "-std=c++20",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wno-unused-parameter",
    "-Wno-missing-field-initializers",
]

cc_library(
    name = "quant_config",
    copts = COPTS,
    visibility = ["//visibility:public"],
)