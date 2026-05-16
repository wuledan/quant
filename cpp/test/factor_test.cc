// factor_test.cc — Tests for FactorRegistry, FactorDAG, FactorComputer, BuiltInFactors
#include "cpp/quant/factor/factor_registry.h"
#include "cpp/quant/factor/factor_dag.h"
#include "cpp/quant/factor/factor_computer.h"
#include "cpp/quant/factor/built_in_factors.h"

#include <gtest/gtest.h>
#include <cmath>
#include <numeric>

namespace quant::factor {
namespace {

// ========================================================================
// FactorRegistry tests
// ========================================================================

TEST(FactorRegistryTest, RegisterAndLookup) {
    FactorRegistry reg;

    FactorMeta meta;
    meta.name = "test_factor";
    meta.description = "A test factor";

    FactorId id = reg.register_factor(std::move(meta),
        [](const auto&) { return std::unordered_map<std::string, std::vector<double>>{}; });

    EXPECT_GT(id, 0u);
    EXPECT_TRUE(reg.has_factor("test_factor"));
    EXPECT_TRUE(reg.has_factor(id));

    const auto* meta_ptr = reg.get_meta(id);
    ASSERT_NE(meta_ptr, nullptr);
    EXPECT_EQ(meta_ptr->name, "test_factor");
    EXPECT_EQ(meta_ptr->description, "A test factor");

    EXPECT_EQ(reg.find_id("test_factor"), id);
    EXPECT_EQ(reg.find_id("nonexistent"), 0u);
}

TEST(FactorRegistryTest, RegisterDuplicateOverwrites) {
    FactorRegistry reg;

    FactorMeta m1, m2;
    m1.name = "dup";
    m2.name = "dup";

    FactorId id1 = reg.register_factor(std::move(m1), nullptr);
    FactorId id2 = reg.register_factor(std::move(m2), nullptr);

    // Second register should overwrite first (same name)
    EXPECT_NE(id1, id2);
    EXPECT_EQ(reg.find_id("dup"), id2);
    EXPECT_FALSE(reg.has_factor(id1));
    EXPECT_TRUE(reg.has_factor(id2));
}

TEST(FactorRegistryTest, Unregister) {
    FactorRegistry reg;

    FactorMeta meta;
    meta.name = "to_remove";
    FactorId id = reg.register_factor(std::move(meta), nullptr);

    EXPECT_TRUE(reg.unregister_factor(id));
    EXPECT_FALSE(reg.has_factor("to_remove"));
    EXPECT_FALSE(reg.unregister_factor(9999));
}

TEST(FactorRegistryTest, ListFactors) {
    FactorRegistry reg;

    for (int i = 0; i < 3; ++i) {
        FactorMeta meta;
        meta.name = "factor_" + std::to_string(i);
        reg.register_factor(std::move(meta), nullptr);
    }

    auto list = reg.list_factors();
    EXPECT_EQ(list.size(), 3u);
    EXPECT_EQ(reg.size(), 3u);
}

// ========================================================================
// FactorDAG tests
// ========================================================================

TEST(FactorDAGTest, SimpleDependency) {
    auto reg = std::make_unique<FactorRegistry>();

    // Register factor_a (no deps) and factor_b (depends on factor_a)
    FactorMeta ma;
    ma.name = "A";
    ma.inputs = {};
    reg->register_factor(std::move(ma), nullptr);

    FactorMeta mb;
    mb.name = "B";
    mb.inputs = {"A"};
    reg->register_factor(std::move(mb), nullptr);

    FactorDAG dag(reg.get());
    dag.build();

    auto validation = dag.validate();
    EXPECT_TRUE(validation.valid);

    auto order = dag.topological_sort();
    ASSERT_EQ(order.size(), 2u);
    // B depends on A, so A must come first
    EXPECT_EQ(order[0], reg->find_id("A"));
    EXPECT_EQ(order[1], reg->find_id("B"));

    // Check dependencies
    auto b_deps = dag.get_dependencies(reg->find_id("B"));
    ASSERT_EQ(b_deps.size(), 1u);
    EXPECT_EQ(b_deps[0], reg->find_id("A"));

    auto a_deps = dag.get_dependents(reg->find_id("A"));
    ASSERT_EQ(a_deps.size(), 1u);
    EXPECT_EQ(a_deps[0], reg->find_id("B"));
}

TEST(FactorDAGTest, CycleDetection) {
    auto reg = std::make_unique<FactorRegistry>();

    FactorMeta ma, mb, mc;
    ma.name = "X"; ma.inputs = {"Z"};
    mb.name = "Y"; mb.inputs = {"X"};
    mc.name = "Z"; mc.inputs = {"Y"};
    reg->register_factor(std::move(ma), nullptr);
    reg->register_factor(std::move(mb), nullptr);
    reg->register_factor(std::move(mc), nullptr);

    FactorDAG dag(reg.get());
    dag.build();

    auto validation = dag.validate();
    EXPECT_FALSE(validation.valid);
    EXPECT_FALSE(validation.cycle_path.empty());
}

TEST(FactorDAGTest, ParallelLevels) {
    auto reg = std::make_unique<FactorRegistry>();

    // A and B have no deps (level 0)
    // C and D depend on A and B (level 1)
    FactorMeta ma, mb, mc, md;
    ma.name = "A"; ma.inputs = {};
    mb.name = "B"; mb.inputs = {};
    mc.name = "C"; mc.inputs = {"A", "B"};
    md.name = "D"; md.inputs = {"A", "B"};
    reg->register_factor(std::move(ma), nullptr);
    reg->register_factor(std::move(mb), nullptr);
    reg->register_factor(std::move(mc), nullptr);
    reg->register_factor(std::move(md), nullptr);

    FactorDAG dag(reg.get());
    dag.build();

    auto levels = dag.parallel_levels();
    // Should have 2+ levels (A,B in first, C,D in second)
    EXPECT_GE(levels.size(), 2u);
    // First level should have no dependencies
    EXPECT_EQ(levels[0].size(), 2u);
}

// ========================================================================
// BuiltInFactors tests
// ========================================================================

TEST(BuiltInFactorsTest, MA_Simple) {
    std::vector<double> values = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto result = BuiltInFactors::ma(values, 3);

    EXPECT_EQ(result.size(), 10u);
    EXPECT_TRUE(std::isnan(result[0]));
    EXPECT_TRUE(std::isnan(result[1]));
    // First 3-period MA = (1+2+3)/3 = 2
    EXPECT_NEAR(result[2], 2.0, 1e-10);
    EXPECT_NEAR(result[9], (8+9+10)/3.0, 1e-10);
}

TEST(BuiltInFactorsTest, MA_Empty) {
    std::vector<double> empty;
    EXPECT_TRUE(BuiltInFactors::ma(empty, 10).empty());
}

TEST(BuiltInFactorsTest, EMA_Simple) {
    std::vector<double> values = {1, 2, 3, 4, 5};
    auto result = BuiltInFactors::ema(values, 3);

    EXPECT_EQ(result.size(), 5u);
    EXPECT_TRUE(std::isnan(result[0]));
    EXPECT_TRUE(std::isnan(result[1]));
    // First EMA value = SMA = (1+2+3)/3 = 2
    EXPECT_NEAR(result[2], 2.0, 1e-10);
}

TEST(BuiltInFactorsTest, RSI_Basic) {
    // Monotonically increasing → RSI should be 100
    std::vector<double> up = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                              21, 22, 23, 24, 25, 26, 27, 28, 29, 30};
    auto rsi_up = BuiltInFactors::rsi(up, 14);
    EXPECT_GT(rsi_up.back(), 90.0);  // Should be near 100

    // Monotonically decreasing → RSI should be 0
    std::vector<double> down = {30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20,
                                19, 18, 17, 16, 15, 14, 13, 12, 11, 10};
    auto rsi_down = BuiltInFactors::rsi(down, 14);
    EXPECT_LT(rsi_down.back(), 10.0);  // Should be near 0
}

TEST(BuiltInFactorsTest, MACD_Basic) {
    std::vector<double> values(50, 100.0);  // Constant values
    auto result = BuiltInFactors::macd(values, 12, 26, 9);

    EXPECT_EQ(result.macd_line.size(), 50u);
    EXPECT_EQ(result.signal_line.size(), 50u);
    EXPECT_EQ(result.histogram.size(), 50u);

    // With constant input, MACD line should be 0
    EXPECT_NEAR(result.macd_line.back(), 0.0, 1e-10);
    EXPECT_NEAR(result.signal_line.back(), 0.0, 1e-10);
    EXPECT_NEAR(result.histogram.back(), 0.0, 1e-10);
}

TEST(BuiltInFactorsTest, Bollinger_Basic) {
    std::vector<double> values = {10, 12, 11, 13, 10, 12, 11, 13, 10, 12,
                                   11, 13, 10, 12, 11, 13, 10, 12, 11, 13};
    auto result = BuiltInFactors::bollinger(values, 5, 2.0);

    EXPECT_EQ(result.upper.size(), 20u);
    EXPECT_EQ(result.middle.size(), 20u);
    EXPECT_EQ(result.lower.size(), 20u);

    // Middle = MA, Upper should be > Middle, Lower should be < Middle
    for (size_t i = 4; i < 20; ++i) {
        EXPECT_GT(result.upper[i], result.middle[i]);
        EXPECT_LT(result.lower[i], result.middle[i]);
    }
}

// ========================================================================
// FactorComputer tests
// ========================================================================

TEST(FactorComputerTest, ComputeSimpleFactor) {
    auto reg = std::make_unique<FactorRegistry>();

    // Register a simple add factor: C = A + B
    FactorMeta meta;
    meta.name = "C";
    meta.inputs = {"A", "B"};
    meta.outputs = {"C"};
    reg->register_factor(std::move(meta),
        [](const std::unordered_map<std::string, std::vector<double>>& inputs) {
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& a = inputs.at("A");
            const auto& b = inputs.at("B");
            out["C"].resize(a.size());
            for (size_t i = 0; i < a.size(); ++i) {
                out["C"][i] = a[i] + b[i];
            }
            return out;
        });

    auto dag = std::make_unique<FactorDAG>(reg.get());
    FactorComputer computer(std::move(reg), std::move(dag));

    std::unordered_map<std::string, std::vector<double>> input_data;
    input_data["A"] = {1.0, 2.0, 3.0};
    input_data["B"] = {10.0, 20.0, 30.0};

    auto result = computer.compute_factor("C", input_data);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.outputs.contains("C"));
    EXPECT_EQ(result.outputs["C"].size(), 3u);
    EXPECT_DOUBLE_EQ(result.outputs["C"][0], 11.0);
    EXPECT_DOUBLE_EQ(result.outputs["C"][1], 22.0);
    EXPECT_DOUBLE_EQ(result.outputs["C"][2], 33.0);
}

TEST(FactorComputerTest, ComputeAll) {
    auto reg = std::make_unique<FactorRegistry>();

    FactorMeta ma;
    ma.name = "SUM";
    ma.inputs = {"A", "B"};
    reg->register_factor(std::move(ma),
        [](const auto& inputs) {
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& a = inputs.at("A");
            const auto& b = inputs.at("B");
            out["SUM"].resize(a.size());
            for (size_t i = 0; i < a.size(); ++i) {
                out["SUM"][i] = a[i] + b[i];
            }
            return out;
        });

    FactorMeta mb;
    mb.name = "PRODUCT";
    mb.inputs = {"A", "SUM"};
    reg->register_factor(std::move(mb),
        [](const auto& inputs) {
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& a = inputs.at("A");
            const auto& s = inputs.at("SUM");
            out["PRODUCT"].resize(a.size());
            for (size_t i = 0; i < a.size(); ++i) {
                out["PRODUCT"][i] = a[i] * s[i];
            }
            return out;
        });

    auto dag = std::make_unique<FactorDAG>(reg.get());
    FactorComputer computer(std::move(reg), std::move(dag));

    std::unordered_map<std::string, std::vector<double>> input_data;
    input_data["A"] = {2.0, 3.0, 4.0};
    input_data["B"] = {3.0, 4.0, 5.0};

    auto result = computer.compute_all(input_data);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.outputs.contains("SUM"));
    EXPECT_TRUE(result.outputs.contains("PRODUCT"));
    EXPECT_EQ(result.outputs["SUM"].size(), 3u);
    EXPECT_DOUBLE_EQ(result.outputs["SUM"][0], 5.0);
    EXPECT_DOUBLE_EQ(result.outputs["PRODUCT"][0], 10.0);
}

TEST(FactorComputerTest, CacheAndInvalidate) {
    auto reg = std::make_unique<FactorRegistry>();

    int compute_count = 0;
    FactorMeta meta;
    meta.name = "DOUBLE";
    meta.inputs = {"X"};
    reg->register_factor(std::move(meta),
        [&compute_count](const auto& inputs) {
            compute_count++;
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& x = inputs.at("X");
            out["DOUBLE"].resize(x.size());
            for (size_t i = 0; i < x.size(); ++i) {
                out["DOUBLE"][i] = x[i] * 2;
            }
            return out;
        });

    auto dag = std::make_unique<FactorDAG>(reg.get());
    FactorComputer computer(std::move(reg), std::move(dag));

    std::unordered_map<std::string, std::vector<double>> input_data;
    input_data["X"] = {1.0, 2.0, 3.0};

    auto r1 = computer.compute_factor("DOUBLE", input_data);
    EXPECT_TRUE(r1.success);
    EXPECT_EQ(compute_count, 1);

    // Cached result should be reused
    auto* cached = computer.get_cached("DOUBLE");
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached->size(), 3u);

    // Invalidate and recompute
    computer.invalidate("DOUBLE");
    EXPECT_EQ(computer.get_cached("DOUBLE"), nullptr);

    auto r2 = computer.compute_factor("DOUBLE", input_data);
    EXPECT_TRUE(r2.success);
    EXPECT_EQ(compute_count, 2);

    computer.clear_cache();
    EXPECT_EQ(computer.get_cached("DOUBLE"), nullptr);
}

TEST(FactorComputerTest, MissingInputReturnsError) {
    auto reg = std::make_unique<FactorRegistry>();

    FactorMeta meta;
    meta.name = "NEEDS_DATA";
    meta.inputs = {"MISSING"};
    reg->register_factor(std::move(meta), nullptr);

    auto dag = std::make_unique<FactorDAG>(reg.get());
    FactorComputer computer(std::move(reg), std::move(dag));

    auto result = computer.compute_factor("NEEDS_DATA", {});
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_msg.empty());
}

TEST(FactorComputerTest, BuiltInFactorsFullPipeline) {
    auto reg = std::make_unique<FactorRegistry>();

    // Register all built-in factors
    BuiltInFactors::register_all(*reg);

    auto dag = std::make_unique<FactorDAG>(reg.get());
    FactorComputer computer(std::move(reg), std::move(dag));

    // MA depends on "close"
    std::vector<double> close(50);
    for (int i = 0; i < 50; ++i) {
        close[i] = 100.0 + std::sin(i * 0.1) * 10.0;
    }

    // Compute MA
    auto result = computer.compute_factor("MA", {{"close", close}});
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.outputs.contains("ma"));
    EXPECT_EQ(result.outputs["ma"].size(), 50u);

    // Compute RSI
    auto rsi_result = computer.compute_factor("RSI", {{"close", close}});
    EXPECT_TRUE(rsi_result.success);
    EXPECT_TRUE(rsi_result.outputs.contains("rsi"));

    // Compute MACD
    auto macd_result = computer.compute_factor("MACD", {{"close", close}});
    EXPECT_TRUE(macd_result.success);
    EXPECT_TRUE(macd_result.outputs.contains("macd_line"));
    EXPECT_TRUE(macd_result.outputs.contains("macd_histogram"));

    // Compute Bollinger
    auto boll_result = computer.compute_factor("BOLL", {{"close", close}});
    EXPECT_TRUE(boll_result.success);
    EXPECT_TRUE(boll_result.outputs.contains("boll_upper"));
    EXPECT_TRUE(boll_result.outputs.contains("boll_lower"));
}

TEST(FactorComputerTest, IncrementalUpdate) {
    auto reg = std::make_unique<FactorRegistry>();

    FactorMeta meta;
    meta.name = "ADD";
    meta.inputs = {"X"};
    reg->register_factor(std::move(meta),
        [](const auto& inputs) {
            std::unordered_map<std::string, std::vector<double>> out;
            const auto& x = inputs.at("X");
            out["ADD"].resize(x.size());
            for (size_t i = 0; i < x.size(); ++i) {
                out["ADD"][i] = x[i] + 1.0;
            }
            return out;
        });

    auto dag = std::make_unique<FactorDAG>(reg.get());
    FactorComputer computer(std::move(reg), std::move(dag));

    // Initial compute
    auto r1 = computer.compute_factor("ADD", {{"X", {1.0, 2.0, 3.0}}});
    EXPECT_TRUE(r1.success);
    EXPECT_DOUBLE_EQ(r1.outputs["ADD"][0], 2.0);

    // Incremental update
    auto inc = computer.increment("X", {{"X", {10.0, 20.0}}});
    EXPECT_TRUE(inc.success);
    EXPECT_TRUE(inc.outputs.contains("ADD"));
}

}  // namespace
}  // namespace quant::factor
