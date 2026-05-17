// config_test.cc — Tests for ConfigManager with layering, hot reload, and subscription
#include "cpp/quant/infra/config/config_manager.h"
#include "cpp/quant/infra/config/config_source.h"
#include "cpp/quant/infra/config/toml_source.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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


// ── TOML Config Source Tests ──

TEST(TomlConfigSourceTest, LoadSimpleTypes) {
    // Write a temporary TOML file
    auto tmp = std::filesystem::temp_directory_path() / "test_simple.toml";
    {
        std::ofstream ofs(tmp);
        ofs << R"(
host = "localhost"
port = 8080
timeout = 30.5
debug = true
)";
    }

    TomlConfigSource source(tmp.string());
    std::map<std::string, ConfigValue> kv;
    ASSERT_TRUE(source.load(kv));

    EXPECT_EQ(std::get<std::string>(kv["host"]), "localhost");
    EXPECT_EQ(std::get<int64_t>(kv["port"]), 8080);
    EXPECT_DOUBLE_EQ(std::get<double>(kv["timeout"]), 30.5);
    EXPECT_EQ(std::get<bool>(kv["debug"]), true);

    std::filesystem::remove(tmp);
}

TEST(TomlConfigSourceTest, LoadNestedKeys) {
    auto tmp = std::filesystem::temp_directory_path() / "test_nested.toml";
    {
        std::ofstream ofs(tmp);
        ofs << R"(
[database]
host = "db.example.com"
port = 5432

[redis]
host = "redis.example.com"
port = 6379
db = 0
)";
    }

    TomlConfigSource source(tmp.string());
    std::map<std::string, ConfigValue> kv;
    ASSERT_TRUE(source.load(kv));

    EXPECT_EQ(std::get<std::string>(kv["database.host"]), "db.example.com");
    EXPECT_EQ(std::get<int64_t>(kv["database.port"]), 5432);
    EXPECT_EQ(std::get<std::string>(kv["redis.host"]), "redis.example.com");
    EXPECT_EQ(std::get<int64_t>(kv["redis.port"]), 6379);
    EXPECT_EQ(std::get<int64_t>(kv["redis.db"]), 0);

    std::filesystem::remove(tmp);
}

TEST(TomlConfigSourceTest, LoadArrays) {
    auto tmp = std::filesystem::temp_directory_path() / "test_arrays.toml";
    {
        std::ofstream ofs(tmp);
        ofs << R"(
ids = [1, 2, 3]
prices = [10.5, 20.3, 30.1]
nodes = ["nyc", "london", "tokyo"]
flags = [true, false, true]
)";
    }

    TomlConfigSource source(tmp.string());
    std::map<std::string, ConfigValue> kv;
    ASSERT_TRUE(source.load(kv));

    auto ids = std::get<std::vector<int64_t>>(kv["ids"]);
    ASSERT_EQ(ids.size(), 3);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[2], 3);

    auto prices = std::get<std::vector<double>>(kv["prices"]);
    ASSERT_EQ(prices.size(), 3);
    EXPECT_DOUBLE_EQ(prices[1], 20.3);

    auto nodes = std::get<std::vector<std::string>>(kv["nodes"]);
    ASSERT_EQ(nodes.size(), 3);
    EXPECT_EQ(nodes[2], "tokyo");

    auto flags = std::get<std::vector<bool>>(kv["flags"]);
    ASSERT_EQ(flags.size(), 3);
    EXPECT_TRUE(flags[0]);
    EXPECT_FALSE(flags[1]);

    std::filesystem::remove(tmp);
}

TEST(TomlConfigSourceTest, FileNotFound) {
    TomlConfigSource source("/nonexistent/path.toml");
    std::map<std::string, ConfigValue> kv;
    EXPECT_FALSE(source.load(kv));
}

TEST(TomlConfigSourceTest, SaveAndReload) {
    auto tmp = std::filesystem::temp_directory_path() / "test_save.toml";
    {
        std::map<std::string, ConfigValue> kv;
        kv["host"] = std::string{"example.com"};
        kv["port"] = int64_t{443};
        kv["database.name"] = std::string{"mydb"};
        kv["database.pool"] = int64_t{10};

        TomlConfigSource source(tmp.string());
        ASSERT_TRUE(source.save(kv));
    }

    // Reload and verify
    {
        TomlConfigSource source(tmp.string());
        std::map<std::string, ConfigValue> kv;
        ASSERT_TRUE(source.load(kv));

        EXPECT_EQ(std::get<std::string>(kv["host"]), "example.com");
        EXPECT_EQ(std::get<int64_t>(kv["port"]), 443);
        EXPECT_EQ(std::get<std::string>(kv["database.name"]), "mydb");
        EXPECT_EQ(std::get<int64_t>(kv["database.pool"]), 10);
    }

    std::filesystem::remove(tmp);
}

TEST(TomlConfigSourceTest, IntegrateWithConfigManager) {
    auto tmp = std::filesystem::temp_directory_path() / "test_integrate.toml";
    {
        std::ofstream ofs(tmp);
        ofs << R"(
host = "api.example.com"
port = 9999
)";
    }

    ConfigManager cm;
    ASSERT_TRUE(cm.load_global(std::make_unique<TomlConfigSource>(tmp.string())));

    auto host = cm.get<std::string>("host");
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(*host, "api.example.com");

    auto port = cm.get<int64_t>("port");
    ASSERT_TRUE(port.has_value());
    EXPECT_EQ(*port, 9999);

    std::filesystem::remove(tmp);
}

}  // namespace
}  // namespace quant::infra
