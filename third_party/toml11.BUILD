// toml11.BUILD — Bazel build for toml11 (header-only)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "toml11",
    hdrs = glob(["toml/**/*.hpp"]),
    includes = ["."],
    copts = ["-std=c++20"],
)
