# BUILD - C++ network library

package(default_visibility = ["//visibility:public"])


cc_library(
    name = "network",
    srcs = [
        "tcp_connection.cc",
        "websocket_server.cc",
        "ws_session.cc",
    ],
    hdrs = [
        "tcp_connection.h",
        "websocket_server.h",
        "ws_session.h",
        "network_layer.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        "//cpp/quant/infra:infra",
    ],
)
