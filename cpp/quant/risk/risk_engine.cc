// risk_engine.cc — Risk engine implementation
#include "cpp/quant/risk/risk_engine.h"

#include <chrono>

namespace quant::risk {

void RiskEngine::register_rule(std::unique_ptr<IRiskRule> rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    RuleId id = rule->id();
    rule_order_.push_back(id);
    rules_.emplace(id, std::move(rule));
}

void RiskEngine::unregister_rule(RuleId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.erase(id);
    rule_order_.erase(
        std::remove(rule_order_.begin(), rule_order_.end(), id),
        rule_order_.end());
}

IRiskRule* RiskEngine::find_rule(RuleId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rules_.find(id);
    return it != rules_.end() ? it->second.get() : nullptr;
}

std::vector<IRiskRule*> RiskEngine::all_rules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<IRiskRule*> result;
    result.reserve(rule_order_.size());
    for (auto id : rule_order_) {
        auto it = rules_.find(id);
        if (it != rules_.end() && it->second->is_enabled()) {
            result.push_back(it->second.get());
        }
    }
    return result;
}

RiskEngine::CheckResult RiskEngine::check(const RiskContext& ctx) {
    CheckResult result;
    result.approved = true;

    if (!enabled_.load(std::memory_order_relaxed)) {
        return result;  // Engine disabled, approve everything
    }

    if (is_circuit_break()) {
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_ns - circuit_break_start_ns_ < cb_config_.cooldown_period.count()) {
            result.approved = false;
            result.rule_results.push_back(
                RiskCheckResult::reject(0, "CircuitBreaker",
                    "Trading halted due to circuit breaker", 0, 0));
            total_checks_.fetch_add(1, std::memory_order_relaxed);
            total_rejections_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
        reset_circuit_break();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto id : rule_order_) {
        auto it = rules_.find(id);
        if (it == rules_.end() || !it->second->is_enabled()) continue;

        auto check_result = it->second->check(ctx);
        result.rule_results.push_back(check_result);
        if (!check_result.approved) {
            result.approved = false;
        }
    }

    total_checks_.fetch_add(1, std::memory_order_relaxed);
    if (result.approved) {
        total_approvals_.fetch_add(1, std::memory_order_relaxed);
        consecutive_rejects_.store(0, std::memory_order_relaxed);
    } else {
        total_rejections_.fetch_add(1, std::memory_order_relaxed);
    }
    update_circuit_break(result);

    return result;
}

bool RiskEngine::is_circuit_break() const noexcept {
    return circuit_break_.load(std::memory_order_relaxed);
}

void RiskEngine::reset_circuit_break() noexcept {
    circuit_break_.store(false, std::memory_order_relaxed);
    consecutive_rejects_.store(0, std::memory_order_relaxed);
}

void RiskEngine::update_circuit_break(const CheckResult& result) {
    if (!result.approved) {
        int32_t rejects = consecutive_rejects_.fetch_add(1, std::memory_order_relaxed) + 1;

        // Check circuit breaker conditions
        if (rejects >= cb_config_.max_consecutive_rejects) {
            circuit_break_.store(true, std::memory_order_relaxed);
            circuit_breaks_.fetch_add(1, std::memory_order_relaxed);
            circuit_break_start_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
    } else {
        consecutive_rejects_.store(0, std::memory_order_relaxed);
    }
}

RiskEngineStats RiskEngine::stats() const noexcept {
    RiskEngineStats s;
    s.total_checks = total_checks_.load(std::memory_order_relaxed);
    s.total_approvals = total_approvals_.load(std::memory_order_relaxed);
    s.total_rejections = total_rejections_.load(std::memory_order_relaxed);
    s.circuit_breaks = circuit_breaks_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace quant::risk