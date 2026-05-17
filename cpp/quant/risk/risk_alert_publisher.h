// risk_alert_publisher.h — Publishes risk alerts via EventBus
#pragma once

#include <cstdint>
#include <string>

#include "cpp/quant/event/event_bus.h"
#include "cpp/quant/event/events/risk_alert_event.h"
#include "cpp/quant/risk/risk_rule.h"

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
    void publish(const RiskAlert& alert);

    // Convenience: publish from a RiskCheckResult
    void publish_rejection(const RiskCheckResult& result,
                           AlertSeverity severity = AlertSeverity::kWarning);

private:
    static event::RiskLevel to_risk_level(AlertSeverity sev);
    static event::RuleSeverity to_rule_severity(AlertSeverity sev);

    event::EventBus& event_bus_;
};

}  // namespace quant::risk