// algorithmic_trader.cc — Algorithmic trading strategy implementations
#include "cpp/quant/execution/algorithmic_trader.h"

#include <algorithm>
#include <chrono>

namespace quant::execution {

// ── AlgorithmicTrader base ──
bool AlgorithmicTrader::start() {
    if (running_.exchange(true)) return false;
    start_time_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    OrderRequest req;
    req.symbol = config_.symbol;
    req.side = config_.side;
    req.type = config_.limit_price > 0 ? OrderType::kLimit : OrderType::kMarket;
    req.price = config_.limit_price;
    req.quantity = config_.total_quantity;
    req.tif = TimeInForce::kDay;

    auto result = order_manager_.create_order(req);
    if (result.ok()) {
        parent_order_id_ = result.value();
    }
    return true;
}

void AlgorithmicTrader::stop() noexcept {
    running_.store(false, std::memory_order_relaxed);
}

CoTask<void> AlgorithmicTrader::co_on_fill(const FillReport& report) {
    auto lock = co_await mutex_.co_scoped_lock();
    total_filled_ += report.fill_quantity;
    total_value_ += report.fill_quantity * report.fill_price;
}

void AlgorithmicTrader::on_fill(const FillReport& report) {
    folly::coro::blockingWait(co_on_fill(report));
}

AlgorithmicTrader::Stats AlgorithmicTrader::stats() const noexcept {
    auto lock = folly::coro::blockingWait(mutex_.co_scoped_lock());
    Stats s;
    s.parent_order_id = parent_order_id_;
    s.total_filled = total_filled_;
    s.total_slices = total_slices_;
    s.total_value = total_value_;
    return s;
}

CoTask<AlgorithmicTrader::Stats> AlgorithmicTrader::co_stats() const noexcept {
    auto lock = co_await mutex_.co_scoped_lock();
    Stats s;
    s.parent_order_id = parent_order_id_;
    s.total_filled = total_filled_;
    s.total_slices = total_slices_;
    s.total_value = total_value_;
    co_return s;
}

OrderId AlgorithmicTrader::submit_slice(int64_t quantity) {
    OrderRequest req;
    req.symbol = config_.symbol;
    req.side = config_.side;
    req.type = config_.limit_price > 0 ? OrderType::kLimit : OrderType::kMarket;
    req.price = config_.limit_price;
    req.quantity = quantity;
    req.tif = TimeInForce::kIOC;

    auto result = order_manager_.create_order(req);
    ++total_slices_;
    return result.ok() ? result.value() : 0;
}

CoTask<OrderId> AlgorithmicTrader::co_submit_slice(int64_t quantity) {
    OrderRequest req;
    req.symbol = config_.symbol;
    req.side = config_.side;
    req.type = config_.limit_price > 0 ? OrderType::kLimit : OrderType::kMarket;
    req.price = config_.limit_price;
    req.quantity = quantity;
    req.tif = TimeInForce::kIOC;

    auto result = co_await order_manager_.co_create_order(req);
    ++total_slices_;
    co_return result.ok() ? result.value() : 0;
}

// ── TWAP ──
void TwapTrader::on_tick(int64_t now_ns) {
    folly::coro::blockingWait(co_on_tick(now_ns));
}

CoTask<void> TwapTrader::co_on_tick(int64_t now_ns) {
    if (!running_.load(std::memory_order_relaxed)) co_return;
    if (config_.total_quantity <= total_filled_) co_return;

    auto lock = co_await mutex_.co_scoped_lock();

    int64_t elapsed = now_ns - last_slice_time_ns_;
    if (elapsed < slice_interval_.count()) co_return;

    int64_t remaining = config_.total_quantity - total_filled_;
    int64_t qty = calc_slice_qty(remaining, now_ns);
    if (qty > 0) {
        co_await co_submit_slice(qty);
        last_slice_time_ns_ = now_ns;
    }
}

int64_t TwapTrader::calc_slice_qty(int64_t remaining, int64_t now_ns) {
    if (config_.end_time_ns <= config_.start_time_ns) {
        return std::max(int64_t{1}, remaining / 10);
    }
    auto total_duration = config_.end_time_ns - config_.start_time_ns;
    auto elapsed = now_ns - config_.start_time_ns;
    if (elapsed <= 0) return 0;

    double progress = std::min(1.0, static_cast<double>(elapsed) / total_duration);
    int64_t expected = static_cast<int64_t>(config_.total_quantity * progress);
    int64_t slice = expected - total_filled_;
    return std::clamp(slice, int64_t{0}, remaining);
}

// ── VWAP ──
void VwapTrader::set_volume_profile(const std::vector<double>& weights) {
    volume_weights_ = weights;
}

void VwapTrader::on_tick(int64_t now_ns) {
    folly::coro::blockingWait(co_on_tick(now_ns));
}

CoTask<void> VwapTrader::co_on_tick(int64_t now_ns) {
    if (!running_.load(std::memory_order_relaxed)) co_return;
    if (config_.total_quantity <= total_filled_) co_return;

    auto lock = co_await mutex_.co_scoped_lock();

    int64_t elapsed = now_ns - last_slice_time_ns_;
    if (elapsed < slice_interval_.count()) co_return;

    int64_t remaining = config_.total_quantity - total_filled_;
    int64_t qty = calc_slice_qty(remaining, now_ns);
    if (qty > 0) {
        co_await co_submit_slice(qty);
        last_slice_time_ns_ = now_ns;
    }
}

int64_t VwapTrader::calc_slice_qty(int64_t remaining, int64_t /*now_ns*/) {
    if (volume_weights_.empty()) {
        return std::max(int64_t{1}, remaining / 10);
    }
    size_t idx = std::min(static_cast<size_t>(total_slices_),
                          volume_weights_.size() - 1);
    double weight = volume_weights_[idx];
    int64_t qty = static_cast<int64_t>(config_.total_quantity * weight);
    return std::min(std::max(int64_t{1}, qty), remaining);
}

// ── PoV ──
void PovTrader::on_tick(int64_t now_ns) {
    folly::coro::blockingWait(co_on_tick(now_ns));
}

CoTask<void> PovTrader::co_on_tick(int64_t now_ns) {
    if (!running_.load(std::memory_order_relaxed)) co_return;
    if (config_.total_quantity <= total_filled_) co_return;

    auto lock = co_await mutex_.co_scoped_lock();

    int64_t remaining = config_.total_quantity - total_filled_;
    int64_t qty = calc_slice_qty(remaining, now_ns);
    if (qty > 0) {
        co_await co_submit_slice(qty);
    }
}

int64_t PovTrader::calc_slice_qty(int64_t remaining, int64_t now_ns) {
    estimated_volume_ = 100000;  // would come from market data
    last_volume_update_ns_ = now_ns;
    int64_t qty = static_cast<int64_t>(estimated_volume_ * participation_rate_);
    return std::min(std::max(int64_t{1}, qty), remaining);
}

}  // namespace quant::execution