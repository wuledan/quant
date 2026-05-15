// memory_pool_test.cc — Tests for tiered memory pool
#include "cpp/quant/infra/memory_pool.h"
#include <gtest/gtest.h>
#include <cstring>
#include <memory_resource>
#include <vector>

namespace quant::infra {
namespace {

TEST(MemoryPoolTest, SmallAllocate) {
    QuantMemoryResource pool;
    void* ptr = pool.allocate(32, 8);
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0xAB, 32);
    pool.deallocate(ptr, 32, 8);
}

TEST(MemoryPoolTest, MediumAllocate) {
    QuantMemoryResource pool;
    void* ptr = pool.allocate(512, 8);
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0xCD, 512);
    pool.deallocate(ptr, 512, 8);
}

TEST(MemoryPoolTest, LargeAllocate) {
    QuantMemoryResource pool;
    void* ptr = pool.allocate(8192, 8);
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0xEF, 8192);
    pool.deallocate(ptr, 8192, 8);
}

TEST(MemoryPoolTest, AlignedAllocate) {
    QuantMemoryResource pool;
    void* ptr = pool.allocate(64, 64);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 64, 0);
    pool.deallocate(ptr, 64, 64);
}

TEST(MemoryPoolTest, MultipleSmallAllocations) {
    QuantMemoryResource pool;
    std::vector<void*> ptrs;
    for (int i = 0; i < 100; ++i) {
        void* ptr = pool.allocate(16, 8);
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }
    for (auto* ptr : ptrs) {
        pool.deallocate(ptr, 16, 8);
    }
}

TEST(MemoryPoolTest, PmrVectorInterop) {
    QuantMemoryResource pool;
    std::pmr::vector<int> vec(&pool);
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_EQ(vec.size(), 3);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
}

TEST(MemoryPoolTest, GlobalResource) {
    auto& pool = global_memory_resource();
    void* ptr = pool.allocate(64, 8);
    ASSERT_NE(ptr, nullptr);
    pool.deallocate(ptr, 64, 8);
}

TEST(MemoryPoolTest, StatsTracking) {
    QuantMemoryResource pool;
    void* ptr = pool.allocate(128, 8);
    pool.deallocate(ptr, 128, 8);

    auto stats = pool.stats();
    EXPECT_GE(stats.alloc_count, 1);
    EXPECT_GE(stats.free_count, 1);
}

TEST(MemoryPoolTest, IsEqual) {
    QuantMemoryResource pool_a;
    QuantMemoryResource pool_b;
    EXPECT_TRUE(pool_a.is_equal(pool_a));
    EXPECT_FALSE(pool_a.is_equal(pool_b));
}

}  // namespace
}  // namespace quant::infra
