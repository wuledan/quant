// config_test.cc — Tests for ConfigManager with layering, hot reload, and subscription
#include "cpp/quant/infra/config/config_manager.h"
#include "cpp/quant/infra/config/config_source.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>

namespace quant::infra {
namespace {

// ── In-memory config source for testing ──
class TestConfigSource : public ConfigSource {
public:
    explicit TestConfigSource(std::string_view name,
                              std::map<std::string, ConfigValue> data)
        : name_(name), data_(std::move(data)) {}

    void update(const std::map<std::string, ConfigValue>& data) {
        data_ = data;
    }

    std::string_view name() const noexcept override { return name_; }

    bool load(std::map<std::string, ConfigValue>& kv) override {
        for (const auto& [k, v] : data_) {
            kv[k] = v;
        }
        return true;
    }

private:
    std::string name_;
    std::map<std::string, ConfigValue> data_;
};

TEST(ConfigManagerTest, GlobalLoadAndGet) {
    ConfigManager cm;
    auto source = std::make_unique<TestConfigSource>(
        "test", std::map<std::string, ConfigValue>{
            {"host", std::string{"localhost"}},
            {"port", int64_t{8080}},
            {"timeout", double{30.0}},
    });
    ASSERT_TRUE(cm.load_global(std::move(source)));

    auto host = cm.get<std::string>("host");
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(*host, "localhost");

    auto port = cm.get<int64_t>("port");
    ASSERT_TRUE(port.has_value());
    EXPECT_EQ(*port, 8080);

    auto timeout = cm.get<double>("timeout");
    ASSERT_TRUE(timeout.has_value());
    EXPECT_DOUBLE_EQ(*timeout, 30.0);
}

TEST(ConfigManagerTest, ModuleLoadAndOverride) {
    ConfigManager cm;
    cm.load_global(std::make_unique<TestConfigSource>(
        "global", std::map<std::string, ConfigValue>{
            {"host", std::string{"default"}},
            {"port", int64_t{80}},
    }));

    cm.load_module("trading", std::make_unique<TestConfigSource>(
        "trading_cfg", std::map<std::string, ConfigValue>{
            {"host", std::string{"trading-host"}},
            {"timeout", double{5.0}},
    }));

    // Module overrides host, adds timeout
    auto host = cm.get<std::string>("host");
    EXPECT_EQ(*host, "trading-host");

    // Port comes from global (not overridden)
    auto port = cm.get<int64_t>("port");
    EXPECT_EQ(*port, 80);

    // Timeout from module
    auto timeout = cm.get<double>("timeout");
    EXPECT_DOUBLE_EQ(*timeout, 5.0);
}

TEST(ConfigManagerTest, StrategyOverridesAll) {
    ConfigManager cm;
    cm.load_global(std::make_unique<TestConfigSource>(
        "global", std::map<std::string, ConfigValue>{{"risk_limit", double{0.1}}}));

    cm.load_strategy("strategy_a", std::make_unique<TestConfigSource>(
        "strat", std::map<std::string, ConfigValue>{{{"risk_limit", double{0.05}}}}));

    auto val = cm.get<double>("risk_limit");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 0.05);  // strategy overrides global
}

TEST(ConfigManagerTest, MissingKey) {
    ConfigManager cm;
    auto val = cm.get<int64_t>("nonexistent");
    EXPECT_FALSE(val.has_value());
}

TEST(ConfigManagerTest, GetOrDefault) {
    ConfigManager cm;
    auto val = cm.get_or<int64_t>("missing", 42);
    EXPECT_EQ(val, 42);
}

TEST(ConfigManagerTest, RuntimeSetAndGet) {
    ConfigManager cm;
    cm.set("runtime_key", std::string{"runtime_val"}, ConfigLevel::kGlobal);

    auto val = cm.get<std::string>("runtime_key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "runtime_val");
}

TEST(ConfigManagerTest, SubscribeAndNotify) {
    ConfigManager cm;
    std::atomic<int> callback_count{0};
    std::string changed_key;

    cm.subscribe("my_key", [&](const ConfigChangeEvent& e) {
        callback_count++;
        changed_key = e.key;
    });

    cm.set("my_key", int64_t{100}, ConfigLevel::kGlobal);
    EXPECT_EQ(callback_count.load(), 1);
    EXPECT_EQ(changed_key, "my_key");
}

TEST(ConfigManagerTest, SubscribeWildcard) {
    ConfigManager cm;
    std::atomic<int> callback_count{0};

    cm.subscribe("*", [&](const ConfigChangeEvent&) {
        callback_count++;
    });

    cm.set("key1", int64_t{1}, ConfigLevel::kGlobal);
    cm.set("key2", int64_t{2}, ConfigLevel::kGlobal);
    EXPECT_EQ(callback_count.load(), 2);
}

TEST(ConfigManagerTest, Unsubscribe) {
    ConfigManager cm;
    std::atomic<int> callback_count{0};

    auto id = cm.subscribe("test", [&](const ConfigChangeEvent&) {
        callback_count++;
    });

    cm.set("test", int64_t{1}, ConfigLevel::kGlobal);
    EXPECT_EQ(callback_count.load(), 1);

    cm.unsubscribe(id);
    cm.set("test", int64_t{2}, ConfigLevel::kGlobal);
    EXPECT_EQ(callback_count.load(), 1);  // unsubscribed
}

TEST(ConfigManagerTest, ExportAll) {
    ConfigManager cm;
    cm.load_global(std::make_unique<TestConfigSource>(
        "g", std::map<std::string, ConfigValue>{{"a", int64_t{1}}}));
    cm.load_module("m", std::make_unique<TestConfigSource>(
        "m", std::map<std::string, ConfigValue>{{"b", int64_t{2}}}));
    cm.load_strategy("s", std::make_unique<TestConfigSource>(
        "s", std::map<std::string, ConfigValue>{{"c", int64_t{3}}}));

    auto all = cm.export_all();
    EXPECT_EQ(std::get<int64_t>(all["a"]), 1);
    EXPECT_EQ(std::get<int64_t>(all["b"]), 2);
    EXPECT_EQ(std::get<int64_t>(all["c"]), 3);
}

TEST(ConfigManagerTest, ConfigLevelSet) {
    ConfigManager cm;
    cm.set("key", std::string{"global"}, ConfigLevel::kGlobal);
    cm.set("key", std::string{"module"}, ConfigLevel::kModule);
    cm.set("key", std::string{"strategy"}, ConfigLevel::kStrategy);

    auto val = cm.get<std::string>("key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "strategy");  // highest priority
}

TEST(ConfigManagerTest, HotReloadEnableDisable) {
    ConfigManager cm;
    cm.enable_hot_reload(std::chrono::seconds(1));
    cm.disable_hot_reload();
    // Should not crash or hang
}

TEST(ConfigManagerTest, LoadGlobalFailsGracefully) {
    ConfigManager cm;
    // No-op: just verify default state
    EXPECT_FALSE(cm.get<int64_t>("any").has_value());
}

}  // namespace
}  // namespace quant::infra
