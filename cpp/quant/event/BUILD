// BUILD — C++ event library

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "event",
    srcs = ["event_bus.cc"],
    hdrs = [
        "event.h",
        "event_bus.h",
        "event_filter.h",
        "typed_event_bus.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        "//cpp/quant/event/events:event_types",
        "//cpp/quant/event/queue:event_queue",
    ],
)

cc_library(
    name = "event_headers",
    hdrs = [
        "event.h",
        "event_bus.h",
        "event_filter.h",
        "typed_event_bus.h",
    ],
    copts = ["-std=c++20"],
    deps = [
        "//cpp/quant/event/events:event_type_headers",
        "//cpp/quant/event/queue:event_queue_headers",
    ],
)