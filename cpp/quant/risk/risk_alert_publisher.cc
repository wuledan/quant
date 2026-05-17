// risk_alert_publisher.cc — RiskAlertPublisher implementation
#include "cpp/quant/risk/risk_alert_publisher.h"

#include <chrono>

namespace quant::risk {

// ── RiskAlertPublisher ──

void RiskAlertPublisher::publish(const RiskAlert& alert) {
    auto event = std::make_unique<event::RiskAlertEvent>();
    event->rule_id = alert.rule_id;
    event->risk_level = to_risk_level(alert.severity);
    event->severity = to_rule_severity(alert.severity);
    event->message = alert.message;
    event->set_timestamp_us(alert.timestamp_ns / 1000);
    event_bus_.publish(std::move(event));
}

void RiskAlertPublisher::publish_rejection(const RiskCheckResult& result,
                                            AlertSeverity severity) {
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

// ── Static helpers ──

event::RiskLevel RiskAlertPublisher::to_risk_level(AlertSeverity sev) {
    switch (sev) {
        case AlertSeverity::kInfo:     return event::RiskLevel::kGreen;
        case AlertSeverity::kWarning:  return event::RiskLevel::kYellow;
        case AlertSeverity::kError:    return event::RiskLevel::kRed;
        case AlertSeverity::kCritical:  return event::RiskLevel::kRed;
        default:                       return event::RiskLevel::kYellow;
    }
}

event::RuleSeverity RiskAlertPublisher::to_rule_severity(AlertSeverity sev) {
    switch (sev) {
        case AlertSeverity::kInfo:     return event::RuleSeverity::kInfo;
        case AlertSeverity::kWarning:  return event::RuleSeverity::kWarning;
        case AlertSeverity::kError:    return event::RuleSeverity::kBlock;
        case AlertSeverity::kCritical:  return event::RuleSeverity::kCircuit;
        default:                       return event::RuleSeverity::kWarning;
    }
}

}  // namespace quant::risk