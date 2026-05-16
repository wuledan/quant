# BUILD - C++ event library

package(default_visibility = ["//visibility:public"])


# event.h is a standalone target to avoid circular deps with event types
cc_library(
    name = "event_base",
    hdrs = ["event.h"],
    copts = ["-std=c++20"],
)

cc_library(
    name = "event",
    srcs = ["event_bus.cc"],
    hdrs = [
        "event_bus.h",
        "event_filter.h",
        "typed_event_bus.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        ":event_base",
        "//cpp/quant/event/events:event_types",
        "//cpp/quant/event/queue:event_queue",
    ],
)

cc_library(
    name = "event_headers",
    hdrs = [
        "event_bus.h",
        "event_filter.h",
        "typed_event_bus.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        ":event_base",
        "//cpp/quant/event/events:event_type_headers",
        "//cpp/quant/event/queue:event_queue_headers",
    ],
)

# Event types only need event_base, not full event_headers
cc_library(
    name = "event_base_headers",
    hdrs = ["event.h"],
    copts = ["-std=c++20"],
)
