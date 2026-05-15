package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # MIT

cc_library(
    name = "yaml_cpp",
    srcs = glob([
        "src/**/*.cpp",
        "src/**/*.cc",
    ]),
    hdrs = glob([
        "include/**/*.h",
        "include/**/*.hpp",
        "src/**/*.h",
    ]),
    includes = ["include"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)
