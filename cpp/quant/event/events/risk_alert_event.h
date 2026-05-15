// risk_alert_event.h — Risk alert notification
#pragma once

#include <cstdint>
#include <string>

#include "cpp/quant/event/event.h"

namespace quant::event {

using RuleId = uint32_t;

enum class RiskLevel : uint8_t {
    kGreen  = 0,
    kYellow = 1,
    kRed    = 2,
};

enum class RuleSeverity : uint8_t {
    kInfo    = 0,
    kWarning = 1,
    kBlock   = 2,
    kCircuit = 3,
};

class RiskAlertEvent : public Event {
public:
    DEFINE_EVENT_TYPE(RiskAlertEvent, 5);

    RuleId      rule_id;
    RiskLevel   risk_level;
    RuleSeverity severity;
    std::string message;
};

}  // namespace quant::event
