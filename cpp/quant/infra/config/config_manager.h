// config_manager.h — Layered configuration manager with hot reload
#pragma once

#include <any>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cpp/quant/infra/config/config_source.h"
#include "cpp/quant/infra/config/config_validator.h"

namespace quant::infra {

// ── Config level ──
enum class ConfigLevel {
    kGlobal,
    kModule,
    kStrategy,
};

// ── Config change event ──
struct ConfigChangeEvent {
    std::string key;
    ConfigValue old_value;
    ConfigValue new_value;
    ConfigLevel level;
    std::string source;
};

using ConfigCallback = std::function<void(const ConfigChangeEvent&)>;

// ── ConfigManager ──
class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // ── Load ──
    bool load_global(std::unique_ptr<ConfigSource> source);
    bool load_module(std::string_view module, std::unique_ptr<ConfigSource> source);
    bool load_strategy(std::string_view strategy_id, std::unique_ptr<ConfigSource> source);

    // ── Read ──
    template<typename T>
    std::optional<T> get(std::string_view key) const {
        std::shared_lock lock(mutex_);
        // Strategy > Module > Global
        std::string key_str(key);
        auto it = strategy_config_.find(key_str);
        if (it != strategy_config_.end()) {
            return extract_value<T>(it->second);
        }
        it = module_config_.find(key_str);
        if (it != module_config_.end()) {
            return extract_value<T>(it->second);
        }
        it = global_config_.find(key_str);
        if (it != global_config_.end()) {
            return extract_value<T>(it->second);
        }
        return std::nullopt;
    }

    template<typename T>
    T get_or(std::string_view key, T default_val) const {
        auto val = get<T>(key);
        return val.value_or(default_val);
    }

    // ── Set (runtime update) ──
    bool set(std::string_view key, ConfigValue value, ConfigLevel level);

    // ── Subscribe to changes ──
    uint64_t subscribe(std::string_view key_pattern, ConfigCallback callback);
    void unsubscribe(uint64_t id);

    // ── Hot reload ──
    void enable_hot_reload(std::chrono::seconds poll_interval = std::chrono::seconds(5));
    void disable_hot_reload();

    // ── Export all ──
    std::map<std::string, ConfigValue> export_all() const;

private:
    template<typename T>
    static std::optional<T> extract_value(const ConfigValue& v) {
        if (std::holds_alternative<T>(v)) {
            return std::get<T>(v);
        }
        return std::nullopt;
    }

    void notify_change(const ConfigChangeEvent& event);
    void hot_reload_loop();

    // ── Hot reload helpers ──
    void reload_sources(
        std::vector<std::unique_ptr<ConfigSource>>& sources,
        std::map<std::string, ConfigValue>& config,
        ConfigLevel level,
        std::unique_lock<std::shared_mutex>& lock);

    void reload_sources(
        std::unordered_map<std::string,
            std::vector<std::unique_ptr<ConfigSource>>>& sources_map,
        std::map<std::string, ConfigValue>& config,
        ConfigLevel level,
        std::unique_lock<std::shared_mutex>& lock);

    mutable std::shared_mutex mutex_;
    std::map<std::string, ConfigValue> global_config_;
    std::map<std::string, ConfigValue> module_config_;
    std::map<std::string, ConfigValue> strategy_config_;

    std::vector<std::unique_ptr<ConfigSource>> global_sources_;
    std::unordered_map<std::string, std::vector<std::unique_ptr<ConfigSource>>> module_sources_;
    std::unordered_map<std::string, std::vector<std::unique_ptr<ConfigSource>>> strategy_sources_;

    struct Subscription {
        uint64_t id;
        std::string key_pattern;
        ConfigCallback callback;
    };
    std::vector<Subscription> subscriptions_;
    uint64_t next_sub_id_{1};

    std::thread hot_reload_thread_;
    std::atomic<bool> hot_reload_enabled_{false};
    std::chrono::seconds poll_interval_{5};
};

}  // namespace quant::infra
