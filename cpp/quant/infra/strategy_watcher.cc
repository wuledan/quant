// strategy_watcher.cc — Watches etcd for strategy/backtest changes

#include "cpp/quant/infra/strategy_watcher.h"

#include <cstdint>
#include <iostream>

#include <folly/coro/Collect.h>

#include "cpp/quant/ir/ir_graph.h"

namespace quant::infra {

StrategyWatcher::StrategyWatcher(EtcdClient& etcd,
                                 strategy::StrategyEngine& engine)
    : etcd_(etcd), engine_(engine) {}

StrategyWatcher::~StrategyWatcher() {
    stop();
}

// ── Start: load existing + watch ──

CoTask<void> StrategyWatcher::start() {
    // 1. Load all existing strategies from etcd
    auto entries = etcd_.get_prefix(std::string(kStrategyPrefix));
    for (auto& [key, value] : entries) {
        handle_strategy_event(key, value, false);
    }

    std::cout << "[StrategyWatcher] Loaded " << entries.size()
              << " existing strategy entries from etcd\n";

    // 2. Watch for changes on both strategy and backtest prefixes
    auto strategy_watch = etcd_.co_watch_prefix(
        std::string(kStrategyPrefix),
        [this](std::string key, std::string value, bool is_delete) {
            handle_strategy_event(key, std::move(value), is_delete);
        });

    auto backtest_watch = etcd_.co_watch_prefix(
        std::string(kBacktestPrefix),
        [this](std::string key, std::string value, bool is_delete) {
            handle_backtest_event(key, std::move(value), is_delete);
        });

    co_await folly::coro::collectAll(
        std::move(strategy_watch),
        std::move(backtest_watch));
}

void StrategyWatcher::stop() {
    stopped_ = true;
    etcd_.cancel_watches();
}

// ── Key parsing ──

std::string StrategyWatcher::parse_strategy_id(const std::string& key) {
    auto prefix_len = kStrategyPrefix.size();
    if (key.size() <= prefix_len) return "";

    auto rest = key.substr(prefix_len);
    auto slash_pos = rest.find('/');
    if (slash_pos == std::string::npos) return rest;
    return rest.substr(0, slash_pos);
}

std::string StrategyWatcher::parse_key_suffix(const std::string& key) {
    auto prefix_len = kStrategyPrefix.size();
    if (key.size() <= prefix_len) return "";

    auto rest = key.substr(prefix_len);
    auto slash_pos = rest.find('/');
    if (slash_pos == std::string::npos) return "";
    return rest.substr(slash_pos + 1);
}

// ── Event handling ──

void StrategyWatcher::handle_strategy_event(const std::string& key,
                                              const std::string& value,
                                              bool is_delete) {
    auto id = parse_strategy_id(key);
    if (id.empty()) return;

    auto suffix = parse_key_suffix(key);

    if (is_delete) {
        std::cout << "[StrategyWatcher] DELETE strategy id=" << id
                  << " suffix=" << suffix << "\n";
        (void)suffix;
        return;
    }

    // PUT event
    if (suffix == "ir") {
        std::cout << "[StrategyWatcher] PUT strategy IR id=" << id << "\n";
        ir::StrategyGraph graph;
        try {
            graph = ir::StrategyGraph::load_from_json(value);
        } catch (const std::exception& e) {
            std::cerr << "[StrategyWatcher] Failed to parse IR: " << e.what() << "\n";
            return;
        }
        std::string err;
        if (graph.validate(err)) {
            std::string reg_name = graph.strategy_name.empty() ? id : graph.strategy_name;
            auto* existing = engine_.registry().find_by_name(reg_name);
            uint64_t numeric_id;
            if (existing) {
                numeric_id = existing->id;
                engine_.deactivate(numeric_id);
            } else {
                numeric_id = engine_.registry().register_strategy(reg_name, "");
            }
            if (engine_.activate(numeric_id)) {
                std::cout << "[StrategyWatcher] Strategy activated: " << id << "\n";
            } else {
                std::cerr << "[StrategyWatcher] Failed to activate strategy "
                          << id << "\n";
            }
        } else {
            std::cerr << "[StrategyWatcher] Invalid IR for strategy "
                      << id << "\n";
        }
    } else if (suffix == "meta") {
        std::cout << "[StrategyWatcher] PUT strategy meta id=" << id << "\n";
    } else {
        std::cout << "[StrategyWatcher] PUT strategy key=" << key << "\n";
    }
}

void StrategyWatcher::handle_backtest_event(const std::string& key,
                                              const std::string& value,
                                              bool is_delete) {
    if (is_delete) {
        std::cout << "[StrategyWatcher] DELETE backtask key=" << key << "\n";
        return;
    }

    auto prefix_len = kBacktestPrefix.size();
    auto task_id = (key.size() > prefix_len) ? key.substr(prefix_len) : "";

    std::cout << "[StrategyWatcher] PUT backtest task id=" << task_id << "\n";
}

}  // namespace quant::infra
