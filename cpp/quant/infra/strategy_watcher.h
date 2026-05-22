// strategy_watcher.h — Watches etcd for strategy/backtest changes
//
// On startup: reads all existing strategies from etcd and loads them.
// On watch: handles PUT/DELETE events to load/unload strategies.
//
// Key layout in etcd:
//   /quant/strategy/{id}/ir   → JSON IR (FactorDAG)
//   /quant/strategy/{id}/meta → JSON metadata
//   /quant/backtest/task/{id} → JSON backtest task
#pragma once

#include <string>

#include "cpp/quant/infra/etcd_client.h"
#include "cpp/quant/strategy/strategy_engine.h"

namespace quant::infra {

class StrategyWatcher {
public:
    StrategyWatcher(EtcdClient& etcd, strategy::StrategyEngine& engine);
    ~StrategyWatcher();

    StrategyWatcher(const StrategyWatcher&) = delete;
    StrategyWatcher& operator=(const StrategyWatcher&) = delete;

    // Load all existing strategies from etcd, then start watching.
    CoTask<void> start();

    // Stop watching
    void stop();

private:
    static constexpr std::string_view kStrategyPrefix = "/quant/strategy/";
    static constexpr std::string_view kBacktestPrefix = "/quant/backtest/task/";

    static std::string parse_strategy_id(const std::string& key);
    static std::string parse_key_suffix(const std::string& key);

    void handle_strategy_event(const std::string& key,
                                const std::string& value,
                                bool is_delete);

    void handle_backtest_event(const std::string& key,
                                const std::string& value,
                                bool is_delete);

    EtcdClient& etcd_;
    strategy::StrategyEngine& engine_;
    bool stopped_{false};
};

}  // namespace quant::infra
