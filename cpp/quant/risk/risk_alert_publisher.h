// risk_alert_publisher.h — Publishes risk alerts via EventBus
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/risk_alert_event.h"

namespace quant::risk {

// ── Alert severity ──
enum class AlertSeverity : uint8_t {
    kInfo    = 0,
    kWarning = 1,
    kError   = 2,
    kCritical = 3,
};

// ── Risk alert data ──
struct RiskAlert {
    RuleId          rule_id     = 0;
    std::string     rule_name;
    AlertSeverity   severity    = AlertSeverity::kWarning;
    std::string     message;
    double          value       = 0.0;
    double          threshold   = 0.0;
    int64_t         timestamp_ns = 0;
};

// ── Risk alert publisher ──
class RiskAlertPublisher {
public:
    explicit RiskAlertPublisher(event::EventBus& event_bus)
        : event_bus_(event_bus) {}

    // Publish a risk alert as a RiskAlertEvent on the EventBus
    void publish(const RiskAlert& alert) {
        auto event = std::make_unique<event::RiskAlertEvent>();
        event->rule_id = alert.rule_id;
        event->risk_level = to_risk_level(alert.severity);
        event->severity = to_rule_severity(alert.severity);
        event->message = alert.message;
        event->set_timestamp_us(alert.timestamp_ns / 1000);
        event_bus_.publish(std::move(event));
    }

    // Convenience: publish from a RiskCheckResult
    void publish_rejection(const RiskCheckResult& result, AlertSeverity severity = AlertSeverity::kWarning) {
        RiskAlert alert;
        alert.rule_id = result.rule_id;
        alert.rule_name = result.rule_name;
        alert.severity = severity;
        alert.message = result.message;
        alert.value = result.exposure;
        alert.threshold = result.limit;
        alert.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        publish(alert);
    }

private:
    static event::RiskLevel to_risk_level(AlertSeverity sev) {
        switch (sev) {
            case AlertSeverity::kInfo:     return event::RiskLevel::kGreen;
            case AlertSeverity::kWarning:  return event::RiskLevel::kYellow;
            case AlertSeverity::kError:    return event::RiskLevel::kRed;
            case AlertSeverity::kCritical:  return event::RiskLevel::kRed;
            default:                       return event::RiskLevel::kYellow;
        }
    }

    static event::RuleSeverity to_rule_severity(AlertSeverity sev) {
        switch (sev) {
            case AlertSeverity::kInfo:     return event::RuleSeverity::kInfo;
            case AlertSeverity::kWarning:  return event::RuleSeverity::kWarning;
            case AlertSeverity::kError:    return event::RuleSeverity::kBlock;
            case AlertSeverity::kCritical:  return event::RuleSeverity::kCircuit;
            default:                       return event::RuleSeverity::kWarning;
        }
    }

    event::EventBus& event_bus_;
};

}  // namespace quant::risk