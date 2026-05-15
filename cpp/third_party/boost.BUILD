package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # MIT

cc_library(
    name = "boost_lockfree",
    hdrs = glob(["boost/lockfree/**/*.hpp"]),
    includes = ["boost"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "boost",
    hdrs = glob([
        "boost/**/*.hpp",
        "boost/**/*.h",
    ]),
    includes = ["."],
    visibility = ["//visibility:public"],
)
