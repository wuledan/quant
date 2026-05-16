// config_manager.cc — ConfigManager implementation
#include "cpp/quant/infra/config/config_manager.h"

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
    std::unique_lock lock(mutex_);
    bool ok = source->load(global_config_);
    if (ok) {
        global_sources_.push_back(std::move(source));
    }
    return ok;
}

bool ConfigManager::load_module(std::string_view module,
                                  std::unique_ptr<ConfigSource> source) {
    std::unique_lock lock(mutex_);
    bool ok = source->load(module_config_);
    if (ok) {
        module_sources_[std::string(module)].push_back(std::move(source));
    }
    return ok;
}

bool ConfigManager::load_strategy(std::string_view strategy_id,
                                    std::unique_ptr<ConfigSource> source) {
    std::unique_lock lock(mutex_);
    bool ok = source->load(strategy_config_);
    if (ok) {
        strategy_sources_[std::string(strategy_id)].push_back(std::move(source));
    }
    return ok;
}

bool ConfigManager::set(std::string_view key, ConfigValue value, ConfigLevel level) {
    std::unique_lock lock(mutex_);
    auto* target = &global_config_;
    if (level == ConfigLevel::kModule) target = &module_config_;
    else if (level == ConfigLevel::kStrategy) target = &strategy_config_;

    auto it = target->find(std::string(key));
    ConfigValue old_val;
    if (it != target->end()) {
        old_val = it->second;
    }

    (*target)[std::string(key)] = value;

    // Notify
    ConfigChangeEvent event{
        .key = std::string(key),
        .old_value = old_val,
        .new_value = value,
        .level = level,
        .source = "runtime",
    };
    lock.unlock();
    notify_change(event);
    return true;
}

uint64_t ConfigManager::subscribe(std::string_view key_pattern, ConfigCallback callback) {
    std::unique_lock lock(mutex_);
    uint64_t id = next_sub_id_++;
    subscriptions_.push_back({id, std::string(key_pattern), std::move(callback)});
    return id;
}

void ConfigManager::unsubscribe(uint64_t id) {
    std::unique_lock lock(mutex_);
    auto it = std::remove_if(subscriptions_.begin(), subscriptions_.end(),
        [id](const Subscription& s) { return s.id == id; });
    subscriptions_.erase(it, subscriptions_.end());
}

void ConfigManager::notify_change(const ConfigChangeEvent& event) {
    std::shared_lock lock(mutex_);
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

        std::unique_lock lock(mutex_);
        reload_sources(global_sources_, global_config_, ConfigLevel::kGlobal, lock);
        reload_sources(module_sources_, module_config_, ConfigLevel::kModule, lock);
        reload_sources(strategy_sources_, strategy_config_, ConfigLevel::kStrategy, lock);
    }
}

void ConfigManager::reload_sources(
    std::vector<std::unique_ptr<ConfigSource>>& sources,
    std::map<std::string, ConfigValue>& config,
    ConfigLevel level,
    std::unique_lock<std::shared_mutex>>& lock) {
    if (sources.empty()) return;

    auto old_config = config;
    for (auto& source : sources) {
        source->load(config);
    }
    lock.unlock();

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

    lock.lock();
}

void ConfigManager::reload_sources(
    std::unordered_map<std::string,
        std::vector<std::unique_ptr<ConfigSource>>>& sources_map,
    std::map<std::string, ConfigValue>& config,
    ConfigLevel level,
    std::unique_lock<std::shared_mutex>>& lock) {
    if (sources_map.empty()) return;

    auto old_config = config;
    for (auto& [key, sources] : sources_map) {
        for (auto& source : sources) {
            source->load(config);
        }
    }
    lock.unlock();

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

    lock.lock();
}

std::map<std::string, ConfigValue> ConfigManager::export_all() const {
    std::shared_lock lock(mutex_);
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

}  // namespace quant::infra
