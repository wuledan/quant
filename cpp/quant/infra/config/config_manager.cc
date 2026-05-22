// config_manager.cc — ConfigManager implementation (coroutine-aware)
#include "config_manager.h"
#include "coroutine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace quant::infra {

ConfigManager::ConfigManager() = default;

ConfigManager::~ConfigManager() {
    disable_hot_reload();
}

bool ConfigManager::load_global(std::unique_ptr<ConfigSource> source) {
    auto lock = blockingWait(rw_mutex_.co_scoped_lock());
    bool ok = source->load(global_config_);
    if (ok) {
        global_sources_.push_back(std::move(source));
    }
    return ok;
}

bool ConfigManager::load_module(std::string_view module,
                                  std::unique_ptr<ConfigSource> source) {
    auto lock = blockingWait(rw_mutex_.co_scoped_lock());
    bool ok = source->load(module_config_);
    if (ok) {
        module_sources_[std::string(module)].push_back(std::move(source));
    }
    return ok;
}

bool ConfigManager::load_strategy(std::string_view strategy_id,
                                    std::unique_ptr<ConfigSource> source) {
    auto lock = blockingWait(rw_mutex_.co_scoped_lock());
    bool ok = source->load(strategy_config_);
    if (ok) {
        strategy_sources_[std::string(strategy_id)].push_back(std::move(source));
    }
    return ok;
}

bool ConfigManager::set(std::string_view key, ConfigValue value, ConfigLevel level) {
    ConfigChangeEvent event{.source = "runtime"};
    {
        auto lock = blockingWait(rw_mutex_.co_scoped_lock());
        auto* target = &global_config_;
        if (level == ConfigLevel::kModule) target = &module_config_;
        else if (level == ConfigLevel::kStrategy) target = &strategy_config_;

        auto it = target->find(std::string(key));
        ConfigValue old_val;
        if (it != target->end()) {
            old_val = it->second;
        }

        (*target)[std::string(key)] = value;

        event.key = std::string(key);
        event.old_value = old_val;
        event.new_value = value;
        event.level = level;
    }
    notify_change(event);
    return true;
}

uint64_t ConfigManager::subscribe(std::string_view key_pattern, ConfigCallback callback) {
    auto lock = blockingWait(rw_mutex_.co_scoped_lock());
    uint64_t id = next_sub_id_++;
    subscriptions_.push_back({id, std::string(key_pattern), std::move(callback)});
    return id;
}

void ConfigManager::unsubscribe(uint64_t id) {
    auto lock = blockingWait(rw_mutex_.co_scoped_lock());
    auto it = std::remove_if(subscriptions_.begin(), subscriptions_.end(),
        [id](const Subscription& s) { return s.id == id; });
    subscriptions_.erase(it, subscriptions_.end());
}

void ConfigManager::notify_change(const ConfigChangeEvent& event) {
    auto lock = blockingWait(rw_mutex_.co_scoped_shared_lock());
    for (const auto& sub : subscriptions_) {
        if (sub.key_pattern == "*" || sub.key_pattern == event.key) {
            sub.callback(event);
        }
    }
}

void ConfigManager::enable_hot_reload(std::chrono::seconds poll_interval) {
    if (hot_reload_enabled_.exchange(true)) return;
    poll_interval_ = poll_interval;
    hot_reload_thread_ = std::thread([this] { hot_reload_loop(); });
}

void ConfigManager::disable_hot_reload() {
    hot_reload_enabled_.store(false);
    if (hot_reload_thread_.joinable()) {
        hot_reload_thread_.join();
    }
}

void ConfigManager::hot_reload_loop() {
    while (hot_reload_enabled_.load()) {
        std::this_thread::sleep_for(poll_interval_);

        reload_sources(global_sources_, global_config_, ConfigLevel::kGlobal);
        reload_sources(module_sources_, module_config_, ConfigLevel::kModule);
        reload_sources(strategy_sources_, strategy_config_, ConfigLevel::kStrategy);
    }
}

void ConfigManager::reload_sources(
    std::vector<std::unique_ptr<ConfigSource>>& sources,
    std::map<std::string, ConfigValue>& config,
    ConfigLevel level) {
    if (sources.empty()) return;

    std::map<std::string, ConfigValue> old_config;
    {
        auto lock = blockingWait(rw_mutex_.co_scoped_lock());
        old_config = config;
        for (auto& source : sources) {
            source->load(config);
        }
    }

    // Notify new/modified keys
    for (const auto& [key, new_val] : config) {
        auto old_it = old_config.find(key);
        if (old_it == old_config.end()) {
            notify_change({key, ConfigValue{}, new_val, level, "hot_reload"});
        } else if (old_it->second != new_val) {
            notify_change({key, old_it->second, new_val, level, "hot_reload"});
        }
    }
    // Notify removed keys
    for (const auto& [key, old_val] : old_config) {
        if (config.find(key) == config.end()) {
            notify_change({key, old_val, ConfigValue{}, level, "hot_reload"});
        }
    }
}

void ConfigManager::reload_sources(
    std::unordered_map<std::string,
        std::vector<std::unique_ptr<ConfigSource>>>& sources_map,
    std::map<std::string, ConfigValue>& config,
    ConfigLevel level) {
    if (sources_map.empty()) return;

    std::map<std::string, ConfigValue> old_config;
    {
        auto lock = blockingWait(rw_mutex_.co_scoped_lock());
        old_config = config;
        for (auto& [key, sources] : sources_map) {
            for (auto& source : sources) {
                source->load(config);
            }
        }
    }

    for (const auto& [key, new_val] : config) {
        auto old_it = old_config.find(key);
        if (old_it == old_config.end()) {
            notify_change({key, ConfigValue{}, new_val, level, "hot_reload"});
        } else if (old_it->second != new_val) {
            notify_change({key, old_it->second, new_val, level, "hot_reload"});
        }
    }
    for (const auto& [key, old_val] : old_config) {
        if (config.find(key) == config.end()) {
            notify_change({key, old_val, ConfigValue{}, level, "hot_reload"});
        }
    }
}

std::map<std::string, ConfigValue> ConfigManager::export_all() const {
    auto lock = blockingWait(rw_mutex_.co_scoped_shared_lock());
    std::map<std::string, ConfigValue> result = global_config_;
    // Module config overrides global
    for (const auto& [key, val] : module_config_) {
        result[key] = val;
    }
    // Strategy config overrides all
    for (const auto& [key, val] : strategy_config_) {
        result[key] = val;
    }
    return result;
}

// ── Coroutine async reload ──

quant::infra::CoTask<bool> ConfigManager::co_reload() {
    auto lock = co_await co_mutex_.co_scoped_lock();

    bool changed = false;

    // Collect old state for comparison
    auto old_global = global_config_;
    auto old_module = module_config_;
    auto old_strategy = strategy_config_;

    // Reload global sources
    for (auto& src : global_sources_) {
        src->load(global_config_);
    }

    // Reload module sources
    for (auto& [mod, srcs] : module_sources_) {
        for (auto& src : srcs) {
            src->load(module_config_);
        }
    }

    // Reload strategy sources
    for (auto& [sid, srcs] : strategy_sources_) {
        for (auto& src : srcs) {
            src->load(strategy_config_);
        }
    }

    // Collect all change notifications under the lock, then fire outside it
    struct ChangeNote {
        std::string key;
        ConfigValue old_value;
        ConfigValue new_value;
        ConfigLevel level;
    };
    std::vector<ChangeNote> notes;

    auto collect = [&](const auto& old_cfg, const auto& new_cfg,
                        ConfigLevel level) {
        for (const auto& [k, v] : new_cfg) {
            auto it = old_cfg.find(k);
            if (it == old_cfg.end() || it->second != v) {
                changed = true;
                notes.push_back({k, (it == old_cfg.end()) ? ConfigValue{} : it->second, v, level});
            }
        }
        for (const auto& [k, v] : old_cfg) {
            if (new_cfg.find(k) == new_cfg.end()) {
                changed = true;
                notes.push_back({k, v, ConfigValue{}, level});
            }
        }
    };

    collect(old_global, global_config_, ConfigLevel::kGlobal);
    collect(old_module, module_config_, ConfigLevel::kModule);
    collect(old_strategy, strategy_config_, ConfigLevel::kStrategy);

    // Release lock before firing callbacks
    lock.unlock();

    for (const auto& n : notes) {
        notify_change({n.key, n.old_value, n.new_value, n.level, "co_reload"});
    }

    co_return changed;
}

}  // namespace quant::infra
