# BUILD - C++ execution engine library

package(default_visibility = ["//visibility:public"])


cc_library(
    name = "execution",
    srcs = [
        "order_manager.cc",
        "order_state_machine.cc",
        "algorithmic_trader.cc",
    ],
    hdrs = [
        "order.h",
        "order_state_machine.h",
        "order_manager.h",
        "algorithmic_trader.h",
        "broker_interface.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        "//cpp/quant/infra:infra",
    ],
)

cc_library(
    name = "execution_headers",
    hdrs = [
        "order.h",
        "order_state_machine.h",
        "order_manager.h",
        "algorithmic_trader.h",
        "broker_interface.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        "//cpp/quant/infra:infra_headers",
    ],
)