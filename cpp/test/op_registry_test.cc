// op_registry_test.cc — Unit tests for OpRegistry
#include <gtest/gtest.h>

#include "cpp/quant/factor/op_registry.h"

using namespace quant::factor;
using FactorComputeFnInput = std::unordered_map<std::string, std::vector<double>>;
using FactorComputeFnOutput = std::unordered_map<std::string, std::vector<double>>;

class OpRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        OpRegistry::register_all_builtin_ops();
    }
};

// ── Test: Register and find operators ──
TEST_F(OpRegistryTest, FindRegisteredOps) {
    EXPECT_NE(OpRegistry::find("SMA"), nullptr);
    EXPECT_NE(OpRegistry::find("EMA"), nullptr);
    EXPECT_NE(OpRegistry::find("RSI"), nullptr);
    EXPECT_NE(OpRegistry::find("CROSS_ABOVE"), nullptr);
    EXPECT_NE(OpRegistry::find("CROSS_BELOW"), nullptr);
    EXPECT_NE(OpRegistry::find("THRESHOLD"), nullptr);
}

// ── Test: Unknown op returns nullptr ──
TEST_F(OpRegistryTest, UnknownOpReturnsNull) {
    EXPECT_EQ(OpRegistry::find("NONEXISTENT"), nullptr);
}

// ── Test: List ops ──
TEST_F(OpRegistryTest, ListOps) {
    auto ops = OpRegistry::list_ops();
    EXPECT_GE(ops.size(), 8u);
}

// ── Test: SMA computation via OpRegistry ──
TEST_F(OpRegistryTest, SMAComputation) {
    auto* factory = OpRegistry::find("SMA");
    ASSERT_NE(factory, nullptr);

    auto compute = (*factory)({{"period", 3.0}});

    FactorComputeFnInput input;
    input["price"] = {1.0, 2.0, 3.0, 4.0, 5.0};
    auto output = compute(input);

    ASSERT_EQ(output.count("value"), 1u);
    const auto& result = output["value"];
    ASSERT_EQ(result.size(), 5u);
    // SMA(3) at index 2: (1+2+3)/3 = 2.0
    EXPECT_DOUBLE_EQ(result[2], 2.0);
    // SMA(3) at index 3: (2+3+4)/3 = 3.0
    EXPECT_DOUBLE_EQ(result[3], 3.0);
    // SMA(3) at index 4: (3+4+5)/3 = 4.0
    EXPECT_DOUBLE_EQ(result[4], 4.0);
}

// ── Test: EMA computation via OpRegistry ──
TEST_F(OpRegistryTest, EMAComputation) {
    auto* factory = OpRegistry::find("EMA");
    ASSERT_NE(factory, nullptr);

    auto compute = (*factory)({{"period", 3.0}});

    FactorComputeFnInput input;
    input["price"] = {10.0, 11.0, 12.0, 13.0};
    auto output = compute(input);

    ASSERT_EQ(output.count("value"), 1u);
    const auto& result = output["value"];
    ASSERT_EQ(result.size(), 4u);
    // EMA should be monotonically increasing for increasing input
    EXPECT_LT(result[0], result[1]);
    EXPECT_LT(result[1], result[2]);
    EXPECT_LT(result[2], result[3]);
}

// ── Test: CROSS_ABOVE computation ──
TEST_F(OpRegistryTest, CrossAboveComputation) {
    auto* factory = OpRegistry::find("CROSS_ABOVE");
    ASSERT_NE(factory, nullptr);

    auto compute = (*factory)({});

    FactorComputeFnInput input;
    input["fast"] = {1.0, 2.0, 3.0, 2.0, 1.0};
    input["slow"] = {2.0, 2.0, 2.0, 2.0, 2.0};
    auto output = compute(input);

    ASSERT_EQ(output.count("value"), 1u);
    const auto& result = output["value"];
    ASSERT_EQ(result.size(), 5u);
    // Cross above at index 2: fast goes from 2→3 (above slow=2)
    EXPECT_DOUBLE_EQ(result[2], 1.0);
    // No cross at other indices
    EXPECT_DOUBLE_EQ(result[1], 0.0);
    EXPECT_DOUBLE_EQ(result[3], 0.0);
}

// ── Test: CROSS_BELOW computation ──
TEST_F(OpRegistryTest, CrossBelowComputation) {
    auto* factory = OpRegistry::find("CROSS_BELOW");
    ASSERT_NE(factory, nullptr);

    auto compute = (*factory)({});

    FactorComputeFnInput input;
    input["fast"] = {3.0, 2.0, 1.0, 2.0, 3.0};
    input["slow"] = {2.0, 2.0, 2.0, 2.0, 2.0};
    auto output = compute(input);

    ASSERT_EQ(output.count("value"), 1u);
    const auto& result = output["value"];
    ASSERT_EQ(result.size(), 5u);
    // Cross below at index 2: fast goes from 2→1 (below slow=2)
    EXPECT_DOUBLE_EQ(result[2], -1.0);
}

// ── Test: THRESHOLD computation ──
TEST_F(OpRegistryTest, ThresholdComputation) {
    auto* factory = OpRegistry::find("THRESHOLD");
    ASSERT_NE(factory, nullptr);

    auto compute = (*factory)({{"threshold", 70.0}});

    FactorComputeFnInput input;
    input["signal"] = {50.0, 60.0, 75.0, 80.0, 65.0};
    auto output = compute(input);

    ASSERT_EQ(output.count("value"), 1u);
    const auto& result = output["value"];
    ASSERT_EQ(result.size(), 5u);
    EXPECT_DOUBLE_EQ(result[0], 0.0);
    EXPECT_DOUBLE_EQ(result[1], 0.0);
    EXPECT_DOUBLE_EQ(result[2], 1.0);
    EXPECT_DOUBLE_EQ(result[3], 1.0);
    EXPECT_DOUBLE_EQ(result[4], 0.0);
}

// ── Test: AND computation ──
TEST_F(OpRegistryTest, AndComputation) {
    auto* factory = OpRegistry::find("AND");
    ASSERT_NE(factory, nullptr);

    auto compute = (*factory)({});

    FactorComputeFnInput input;
    input["a"] = {1.0, 0.0, 1.0, 0.0};
    input["b"] = {1.0, 1.0, 0.0, 0.0};
    auto output = compute(input);

    ASSERT_EQ(output.count("value"), 1u);
    const auto& result = output["value"];
    ASSERT_EQ(result.size(), 4u);
    EXPECT_DOUBLE_EQ(result[0], 1.0);
    EXPECT_DOUBLE_EQ(result[1], 0.0);
    EXPECT_DOUBLE_EQ(result[2], 0.0);
    EXPECT_DOUBLE_EQ(result[3], 0.0);
}

// ── Test: RSI computation ──
TEST_F(OpRegistryTest, RSIComputation) {
    auto* factory = OpRegistry::find("RSI");
    ASSERT_NE(factory, nullptr);

    auto compute = (*factory)({{"period", 5.0}});

    FactorComputeFnInput input;
    // Rising prices → RSI should be high
    input["price"] = {100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0};
    auto output = compute(input);

    ASSERT_EQ(output.count("value"), 1u);
    const auto& result = output["value"];
    ASSERT_EQ(result.size(), 7u);
    // RSI should be close to 100 for monotonically rising prices
    EXPECT_GT(result[5], 90.0);
}
