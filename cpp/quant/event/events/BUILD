// BUILD — C++ event type definitions

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "event_types",
    hdrs = [
        "market_data_event.h",
        "kline_event.h",
        "trade_signal_event.h",
        "order_report_event.h",
        "risk_alert_event.h",
        "factor_update_event.h",
    ],
    copts = ["-std=c++20"],
    deps = ["//cpp/quant/event:event_headers"],
)

cc_library(
    name = "event_type_headers",
    hdrs = [
        "market_data_event.h",
        "kline_event.h",
        "trade_signal_event.h",
        "order_report_event.h",
        "risk_alert_event.h",
        "factor_update_event.h",
    ],
    copts = ["-std=c++20"],
    deps = ["//cpp/quant/event:event_headers"],
)