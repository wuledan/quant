// risk_engine.h — Risk engine with rule registration, execution, and circuit breaker
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/risk/risk_rule.h"

namespace quant::risk {

using infra::CoTask;

// ── Circuit breaker config ──
struct CircuitBreakerConfig {
    double      drawdown_threshold   = 0.10;   // 10% drawdown triggers halt
    double      loss_threshold      = 0.05;    // 5% daily loss triggers halt
    int32_t     max_consecutive_rejects = 3;   // consecutive rejects before halt
    std::chrono::milliseconds cooldown_period{300000};  // 5 min cooldown
};

// ── Risk engine stats ──
struct RiskEngineStats {
    uint64_t    total_checks     = 0;
    uint64_t    total_approvals  = 0;
    uint64_t    total_rejections  = 0;
    uint64_t    circuit_breaks   = 0;
};

// ── Risk engine ──
class RiskEngine {
public:
    explicit RiskEngine(const CircuitBreakerConfig& cb_cfg = {})
        : cb_config_(cb_cfg) {}

    ~RiskEngine() = default;

    // Disable copy
    RiskEngine(const RiskEngine&) = delete;
    RiskEngine& operator=(const RiskEngine&) = delete;

    // ── Rule management (sync) ──
    void register_rule(std::unique_ptr<IRiskRule> rule);
    void unregister_rule(RuleId id);
    IRiskRule* find_rule(RuleId id) const;
    std::vector<IRiskRule*> all_rules() const;

    // ── Rule management (coroutine) ──
    CoTask<void> co_register_rule(std::unique_ptr<IRiskRule> rule);
    CoTask<void> co_unregister_rule(RuleId id);
    CoTask<IRiskRule*> co_find_rule(RuleId id) const;
    CoTask<std::vector<IRiskRule*>> co_all_rules() const;

    // ── Risk check (sync) ──
    // If any rule rejects, the overall result is rejection.
    // The results vector contains all individual rule results.
    struct CheckResult {
        bool                    approved = true;
        std::vector<RiskCheckResult> rule_results;
    };

    CheckResult check(const RiskContext& ctx);

    // ── Risk check (coroutine) ──
    CoTask<CheckResult> co_check(const RiskContext& ctx);

    // ── Circuit breaker ──
    bool is_circuit_break() const noexcept;
    void reset_circuit_break() noexcept;
    const CircuitBreakerConfig& circuit_breaker_config() const noexcept { return cb_config_; }

    // ── Enable/disable ──
    void enable() noexcept { enabled_.store(true, std::memory_order_relaxed); }
    void disable() noexcept { enabled_.store(false, std::memory_order_relaxed); }
    bool is_enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    // ── Stats ──
    RiskEngineStats stats() const noexcept;

private:
    void update_circuit_break(const CheckResult& result);

    mutable infra::CoMutex mutex_;
    std::unordered_map<RuleId, std::unique_ptr<IRiskRule>> rules_;
    std::vector<RuleId> rule_order_;

    CircuitBreakerConfig cb_config_;
    std::atomic<bool>    enabled_{true};
    std::atomic<bool>    circuit_break_{false};
    std::atomic<int32_t> consecutive_rejects_{0};
    int64_t              circuit_break_start_ns_ = 0;

    // Stats
    std::atomic<uint64_t> total_checks_{0};
    std::atomic<uint64_t> total_approvals_{0};
    std::atomic<uint64_t> total_rejections_{0};
    std::atomic<uint64_t> circuit_breaks_{0};
};

}  // namespace quant::risk