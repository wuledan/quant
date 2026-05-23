// strategy_watcher_test.cc — Tests for StrategyRegistry find_by_id
#include <gtest/gtest.h>

#include "cpp/quant/strategy/strategy_registry.h"

namespace strat = quant::strategy;

class StrategyRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        id1_ = registry_.register_strategy("ma_cross", "/graphs/ma.graph");
        id2_ = registry_.register_strategy("rsi_strat", "/graphs/rsi.graph",
                                           {{"period", 14.0}});
    }

    strat::StrategyRegistry registry_;
    uint64_t id1_;
    uint64_t id2_;
};

TEST_F(StrategyRegistryTest, FindByIdExisting) {
    auto* entry = registry_.find_by_id(id1_);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->id, id1_);
    EXPECT_EQ(entry->name, "ma_cross");
    EXPECT_EQ(entry->graph_path, "/graphs/ma.graph");
}

TEST_F(StrategyRegistryTest, FindByIdNonExistent) {
    auto* entry = registry_.find_by_id(99999);
    EXPECT_EQ(entry, nullptr);
}

TEST_F(StrategyRegistryTest, FindByIdAfterRegister) {
    uint64_t id3 = registry_.register_strategy("test3", "/graphs/t3.graph");
    auto* by_id = registry_.find_by_id(id3);
    ASSERT_NE(by_id, nullptr);
    EXPECT_EQ(by_id->name, "test3");

    auto* by_name = registry_.find_by_name("test3");
    ASSERT_NE(by_name, nullptr);
    EXPECT_EQ(by_name->id, id3);
    EXPECT_EQ(by_name, by_id);
}

TEST_F(StrategyRegistryTest, FindByIdAfterRemove) {
    auto* before = registry_.find_by_id(id1_);
    ASSERT_NE(before, nullptr);

    registry_.remove_strategy(id1_);

    auto* after = registry_.find_by_id(id1_);
    EXPECT_EQ(after, nullptr);
}

TEST_F(StrategyRegistryTest, FindByAndByNameConsistent) {
    auto* by_id = registry_.find_by_id(id2_);
    auto* by_name = registry_.find_by_name("rsi_strat");
    ASSERT_NE(by_id, nullptr);
    ASSERT_NE(by_name, nullptr);
    EXPECT_EQ(by_id, by_name);
    EXPECT_EQ(by_id->id, by_name->id);
    EXPECT_EQ(by_id->name, by_name->name);
}

TEST_F(StrategyRegistryTest, FindByIdZero) {
    // ID 0 is never assigned (next_id_ starts at 1)
    auto* entry = registry_.find_by_id(0);
    EXPECT_EQ(entry, nullptr);
}

// ── StrategyWatcher key parsing tests ──

#include "cpp/quant/infra/strategy_watcher.h"

namespace sw_test = quant::infra;

TEST(StrategyWatcherParseTest, ParseStrategyIdNormal) {
    auto id = sw_test::StrategyWatcher::parse_strategy_id("/quant/strategy/42/ir");
    EXPECT_EQ(id, "42");
}

TEST(StrategyWatcherParseTest, ParseStrategyIdNoSuffix) {
    auto id = sw_test::StrategyWatcher::parse_strategy_id("/quant/strategy/42");
    EXPECT_EQ(id, "42");
}

TEST(StrategyWatcherParseTest, ParseStrategyIdEmpty) {
    auto id = sw_test::StrategyWatcher::parse_strategy_id("/quant/strategy/");
    EXPECT_EQ(id, "");
}

TEST(StrategyWatcherParseTest, ParseStrategyIdPrefixOnly) {
    auto id = sw_test::StrategyWatcher::parse_strategy_id("/quant/strategy");
    EXPECT_EQ(id, "");
}

TEST(StrategyWatcherParseTest, ParseKeySuffixNormal) {
    auto suffix = sw_test::StrategyWatcher::parse_key_suffix("/quant/strategy/42/ir");
    EXPECT_EQ(suffix, "ir");
}

TEST(StrategyWatcherParseTest, ParseKeySuffixMeta) {
    auto suffix = sw_test::StrategyWatcher::parse_key_suffix("/quant/strategy/99/meta");
    EXPECT_EQ(suffix, "meta");
}

TEST(StrategyWatcherParseTest, ParseKeySuffixNoSuffix) {
    auto suffix = sw_test::StrategyWatcher::parse_key_suffix("/quant/strategy/42");
    EXPECT_EQ(suffix, "");
}

TEST(StrategyWatcherParseTest, ParseKeySuffixEmpty) {
    auto suffix = sw_test::StrategyWatcher::parse_key_suffix("/quant/strategy/");
    EXPECT_EQ(suffix, "");
}

TEST(StrategyWatcherParseTest, ParseKeySuffixPrefixOnly) {
    auto suffix = sw_test::StrategyWatcher::parse_key_suffix("/quant/strategy");
    EXPECT_EQ(suffix, "");
}

TEST(StrategyWatcherParseTest, ParseKeySuffixMultiSegment) {
    auto suffix = sw_test::StrategyWatcher::parse_key_suffix("/quant/strategy/7/sub/dir");
    EXPECT_EQ(suffix, "sub/dir");
}
