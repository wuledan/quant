// boost.BUILD — Bazel build file for Boost (headers only for lockfree)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "lockfree",
    hdrs = glob([
        "boost/lockfree/**/*.hpp",
        "boost/lockfree/**/*.h",
    ]),
    includes = ["."],
    copts = ["-std=c++20"],
    defines = ["BOOST_ALL_NO_LIB"],
)

cc_library(
    name = "boost",
    hdrs = glob([
        "boost/**/*.hpp",
        "boost/**/*.h",
    ]),
    includes = ["."],
    copts = ["-std=c++20"],
    defines = ["BOOST_ALL_NO_LIB"],
)
