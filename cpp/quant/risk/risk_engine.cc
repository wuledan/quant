// risk_engine.cc — Risk engine implementation
#include "cpp/quant/risk/risk_engine.h"

#include <chrono>

namespace quant::risk {

// ── Coroutine methods ──

CoTask<void> RiskEngine::co_register_rule(std::unique_ptr<IRiskRule> rule) {
    auto lock = co_await mutex_.co_scoped_lock();
    RuleId id = rule->id();
    rule_order_.push_back(id);
    rules_.emplace(id, std::move(rule));
}

CoTask<void> RiskEngine::co_unregister_rule(RuleId id) {
    auto lock = co_await mutex_.co_scoped_lock();
    rules_.erase(id);
    rule_order_.erase(
        std::remove(rule_order_.begin(), rule_order_.end(), id),
        rule_order_.end());
}

CoTask<IRiskRule*> RiskEngine::co_find_rule(RuleId id) const {
    auto lock = co_await mutex_.co_scoped_lock();
    auto it = rules_.find(id);
    co_return it != rules_.end() ? it->second.get() : nullptr;
}

CoTask<std::vector<IRiskRule*>> RiskEngine::co_all_rules() const {
    auto lock = co_await mutex_.co_scoped_lock();
    std::vector<IRiskRule*> result;
    result.reserve(rule_order_.size());
    for (auto id : rule_order_) {
        auto it = rules_.find(id);
        if (it != rules_.end() && it->second->is_enabled()) {
            result.push_back(it->second.get());
        }
    }
    co_return result;
}

CoTask<RiskEngine::CheckResult> RiskEngine::co_check(const RiskContext& ctx) {
    CheckResult result;
    result.approved = true;

    if (!enabled_.load(std::memory_order_relaxed)) {
        co_return result;
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
            co_return result;
        }
        reset_circuit_break();
    }

    {
        auto lock = co_await mutex_.co_scoped_lock();
        for (auto id : rule_order_) {
            auto it = rules_.find(id);
            if (it == rules_.end() || !it->second->is_enabled()) continue;

            auto check_result = it->second->check(ctx);
            result.rule_results.push_back(check_result);
            if (!check_result.approved) {
                result.approved = false;
            }
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

    co_return result;
}

// ── Sync wrappers ──

void RiskEngine::register_rule(std::unique_ptr<IRiskRule> rule) {
    folly::coro::blockingWait(co_register_rule(std::move(rule)));
}

void RiskEngine::unregister_rule(RuleId id) {
    folly::coro::blockingWait(co_unregister_rule(id));
}

IRiskRule* RiskEngine::find_rule(RuleId id) const {
    return folly::coro::blockingWait(co_find_rule(id));
}

std::vector<IRiskRule*> RiskEngine::all_rules() const {
    return folly::coro::blockingWait(co_all_rules());
}

RiskEngine::CheckResult RiskEngine::check(const RiskContext& ctx) {
    return folly::coro::blockingWait(co_check(ctx));
}

// ── Circuit breaker ──

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