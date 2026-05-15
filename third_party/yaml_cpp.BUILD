// yaml_cpp.BUILD — Bazel build for yaml-cpp

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "yaml_cpp",
    srcs = glob([
        "src/**/*.cpp",
        "src/**/*.h",
    ]),
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    copts = ["-std=c++20"],
)
