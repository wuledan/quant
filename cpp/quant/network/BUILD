# BUILD - C++ network library

package(default_visibility = ["//visibility:public"])


cc_library(
    name = "network",
    srcs = [
        "tcp_connection.cc",
        "websocket_server.cc",
        "ws_session.cc",
        "co_io.cc",
        "global_executor.cc",
        "global_io.cc",
    ],
    hdrs = [
        "tcp_connection.h",
        "websocket_server.h",
        "ws_session.h",
        "network_layer.h",
        "co_io.h",
        "global_executor.h",
        "global_io.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        "//cpp/quant/infra:infra",
    ],
)
