// algorithmic_trader.h — Algorithmic trading strategies (TWAP/VWAP/PoV)
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cpp/quant/execution/order.h"
#include "cpp/quant/execution/order_manager.h"

namespace quant::execution {

// ── Algo order config ──
struct AlgoOrderConfig {
    OrderSide    side     = OrderSide::kBuy;
    std::string  symbol;
    int64_t      total_quantity = 0;
    int64_t      limit_price    = 0;  // 0 = market
    int64_t      start_time_ns  = 0;  // 0 = immediate
    int64_t      end_time_ns    = 0;
};

// ── Base algo trader ──
class AlgorithmicTrader {
public:
    AlgorithmicTrader(OrderManager& order_manager, const AlgoOrderConfig& config)
        : order_manager_(order_manager), config_(config) {}
    virtual ~AlgorithmicTrader() = default;

    // Disable copy
    AlgorithmicTrader(const AlgorithmicTrader&) = delete;
    AlgorithmicTrader& operator=(const AlgorithmicTrader&) = delete;

    // ── Lifecycle ──
    virtual bool start();
    virtual void stop() noexcept;

    // ── Called periodically (by timer or tick) to generate slices ──
    virtual void on_tick(int64_t now_ns) = 0;

    // ── Called on fill report ──
    virtual void on_fill(const FillReport& report);

    // ── Status ──
    bool is_running() const noexcept { return running_; }
    const AlgoOrderConfig& config() const noexcept { return config_; }

    struct Stats {
        OrderId parent_order_id = 0;
        int64_t total_filled    = 0;
        int64_t total_slices    = 0;
        int64_t total_value     = 0;
    };
    Stats stats() const noexcept;

protected:
    // ── Create and submit a child slice order ──
    virtual OrderId submit_slice(int64_t quantity);

    // ── Calculate quantity for next slice ──
    virtual int64_t calc_slice_qty(int64_t remaining, int64_t now_ns) = 0;

    OrderManager&       order_manager_;
    AlgoOrderConfig     config_;
    std::atomic<bool>   running_{false};
    mutable std::mutex  mutex_;

    OrderId             parent_order_id_ = 0;
    int64_t             total_filled_    = 0;
    int64_t             total_slices_    = 0;
    int64_t             total_value_     = 0;
    int64_t             start_time_ns_   = 0;
};

// ── TWAP: evenly slice over time ──
class TwapTrader : public AlgorithmicTrader {
public:
    TwapTrader(OrderManager& om, const AlgoOrderConfig& config,
               std::chrono::milliseconds slice_interval = std::chrono::seconds(30))
        : AlgorithmicTrader(om, config), slice_interval_(slice_interval) {}

    void on_tick(int64_t now_ns) override;

protected:
    int64_t calc_slice_qty(int64_t remaining, int64_t now_ns) override;

private:
    std::chrono::milliseconds slice_interval_;
    int64_t last_slice_time_ns_ = 0;
};

// ── VWAP: slice based on volume profile ──
class VwapTrader : public AlgorithmicTrader {
public:
    VwapTrader(OrderManager& om, const AlgoOrderConfig& config,
               std::chrono::milliseconds slice_interval = std::chrono::minutes(5))
        : AlgorithmicTrader(om, config), slice_interval_(slice_interval) {}

    void set_volume_profile(const std::vector<double>& weights);

    void on_tick(int64_t now_ns) override;

protected:
    int64_t calc_slice_qty(int64_t remaining, int64_t now_ns) override;

private:
    std::chrono::milliseconds slice_interval_;
    int64_t last_slice_time_ns_ = 0;
    std::vector<double> volume_weights_;
};

// ── PoV: trade at fixed percentage of estimated volume ──
class PovTrader : public AlgorithmicTrader {
public:
    PovTrader(OrderManager& om, const AlgoOrderConfig& config,
               double participation_rate = 0.1)  // 10% default
        : AlgorithmicTrader(om, config), participation_rate_(participation_rate) {}

    void on_tick(int64_t now_ns) override;

    void set_participation_rate(double rate) noexcept {
        participation_rate_ = rate;
    }

protected:
    int64_t calc_slice_qty(int64_t remaining, int64_t now_ns) override;

private:
    double participation_rate_;
    int64_t estimated_volume_ = 0;
    int64_t last_volume_update_ns_ = 0;
};

}  // namespace quant::execution
