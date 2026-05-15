// concurrentqueue.BUILD — Bazel build for moodycamel::ConcurrentQueue

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "concurrentqueue",
    hdrs = [
        "concurrentqueue.h",
        "blockingconcurrentqueue.h",
        "lightweightsemaphore.h",
    ],
    includes = ["."],
    copts = ["-std=c++20"],
)
