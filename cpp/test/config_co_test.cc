// config_co_test.cc — Tests for ConfigManager coroutine features
#include "config_manager.h"
#include "config_source.h"
#include "coroutine.h"

#include <atomic>
#include <chrono>
#include <map>

#include <gtest/gtest.h>
#include <folly/coro/BlockingWait.h>

using namespace quant::infra;

namespace {

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

}

TEST(ConfigCoTest, SyncGetSetStillWorks) {
    ConfigManager cm;
    cm.set("test_key", std::string{"test_val"}, ConfigLevel::kGlobal);

    auto val = cm.get<std::string>("test_key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "test_val");
}

TEST(ConfigCoTest, CoReloadNoChanges) {
    ConfigManager cm;
    auto source = std::make_unique<TestConfigSource>(
        "test", std::map<std::string, ConfigValue>{
            {"host", std::string{"localhost"}},
            {"port", int64_t{8080}},
    });
    ASSERT_TRUE(cm.load_global(std::move(source)));

    bool changed = blockingWait(cm.co_reload());
    EXPECT_FALSE(changed);

    auto host = cm.get<std::string>("host");
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(*host, "localhost");
}

TEST(ConfigCoTest, CoReloadModule) {
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

    auto host = cm.get<std::string>("host");
    EXPECT_EQ(*host, "trading-host");

    auto port = cm.get<int64_t>("port");
    EXPECT_EQ(*port, 80);
}

TEST(ConfigCoTest, SubscribeAndNotify) {
    ConfigManager cm;
    std::atomic<int> callback_count{0};

    cm.subscribe("my_key", [&](const ConfigChangeEvent&) {
        callback_count++;
    });

    cm.set("my_key", int64_t{100}, ConfigLevel::kGlobal);
    EXPECT_EQ(callback_count.load(), 1);
}

TEST(ConfigCoTest, GetOrDefault) {
    ConfigManager cm;
    auto val = cm.get_or<int64_t>("missing", 42);
    EXPECT_EQ(val, 42);
}
