// order_state_machine.cc — Order state machine implementation
#include "cpp/quant/execution/order_state_machine.h"

namespace quant::execution {

// Transition table: from -> to validity
static constexpr bool kTransitionTable[9][9] = {
    //             PN  N   PF  F   C   PC  R   E   S
    /*PN*/       { 0,  1,  0,  0,  0,  0,  1,  0,  0 },  // PendingNew
    /*N*/        { 0,  0,  1,  1,  0,  1,  0,  1,  1 },  // New
    /*PF*/       { 0,  0,  0,  1,  1,  1,  0,  0,  0 },  // PartialFilled
    /*F*/        { 0,  0,  0,  0,  0,  0,  0,  0,  0 },  // Filled (terminal)
    /*C*/        { 0,  0,  0,  0,  0,  0,  0,  0,  0 },  // Cancelled (terminal)
    /*PC*/       { 0,  0,  0,  0,  1,  0,  0,  0,  0 },  // PendingCancel
    /*R*/        { 0,  0,  0,  0,  0,  0,  0,  0,  0 },  // Rejected (terminal)
    /*E*/        { 0,  0,  0,  0,  0,  0,  0,  0,  0 },  // Expired (terminal)
    /*S*/        { 0,  1,  0,  0,  0,  0,  0,  0,  0 },  // Suspended
};

bool OrderStateMachine::is_valid_transition(OrderStatus from, OrderStatus to) noexcept {
    auto f = static_cast<uint8_t>(from);
    auto t = static_cast<uint8_t>(to);
    if (f >= 9 || t >= 9) return false;
    return kTransitionTable[f][t];
}

infra::Result<void> OrderStateMachine::apply_transition(
    OrderStatus from, OrderStatus to) noexcept {
    if (!is_valid_transition(from, to)) {
        return infra::Result<void>(
            infra::ErrorCode::InvalidArgument,
            std::string("Invalid order state transition: ")
            + std::string(to_string(from)) + " -> " + std::string(to_string(to)));
    }
    return infra::Result<void>();
}

}  // namespace quant::execution
