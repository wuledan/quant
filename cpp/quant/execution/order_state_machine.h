// order_state_machine.h — Order state machine with validated transitions
#pragma once

#include "cpp/quant/execution/order.h"
#include "cpp/quant/infra/error_codes.h"

namespace quant::execution {

// ── Allowed transitions ──
//
// PendingNew  ──> New         (broker accepted)
// PendingNew  ──> Rejected    (broker rejected)
// New         ──> PartialFilled
// New         ──> Filled      (immediate fill)
// New         ──> PendingCancel
// New         ──> Expired
// New         ──> Suspended
// PartialFilled ─> Filled
// PartialFilled ─> PendingCancel
// PartialFilled ─> Cancelled   (rest cancelled after partial fill)
// PendingCancel ─> Cancelled   (confirmed)
// Suspended    ──> New         (resumed)

class OrderStateMachine {
public:
    // ── Check if a transition is valid ──
    static bool is_valid_transition(OrderStatus from, OrderStatus to) noexcept;

    // ── Apply transition, returns error on invalid transition ──
    static infra::Result<void> apply_transition(OrderStatus from, OrderStatus to) noexcept;

    // ── Check if status is a terminal state ──
    static bool is_terminal(OrderStatus status) noexcept {
        return status == OrderStatus::kFilled
            || status == OrderStatus::kCancelled
            || status == OrderStatus::kRejected
            || status == OrderStatus::kExpired;
    }

    // ── Check if status is an active state (not terminal, not pending) ──
    static bool is_active(OrderStatus status) noexcept {
        return status == OrderStatus::kNew
            || status == OrderStatus::kPartialFilled
            || status == OrderStatus::kSuspended;
    }

    // ── Check if order can be cancelled ──
    static bool can_cancel(OrderStatus status) noexcept {
        return status == OrderStatus::kNew
            || status == OrderStatus::kPartialFilled
            || status == OrderStatus::kSuspended;
    }

    // ── Check if order can be modified ──
    static bool can_modify(OrderStatus status) noexcept {
        return status == OrderStatus::kNew
            || status == OrderStatus::kSuspended;
    }
};

}  // namespace quant::execution
