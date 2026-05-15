// BUILD — C++ configuration management library

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "config",
    srcs = [
        "config_manager.cc",
        "yaml_source.cc",
        "toml_source.cc",
    ],
    hdrs = [
        "config_source.h",
        "config_manager.h",
        "config_validator.h",
        "yaml_source.h",
        "toml_source.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        "//cpp/quant/infra:infra",
    ],
)
