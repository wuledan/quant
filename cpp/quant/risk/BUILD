# BUILD - C++ risk engine library

package(default_visibility = ["//visibility:public"])


cc_library(
    name = "risk",
    srcs = ["risk_engine.cc"],
    hdrs = [
        "risk_rule.h",
        "risk_engine.h",
        "risk_alert_publisher.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        "//cpp/quant/event:event",
        "//cpp/quant/infra:infra",
    ],
)