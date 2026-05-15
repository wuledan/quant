// object_pool_test.cc — Tests for object pool
#include "cpp/quant/infra/object_pool.h"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

namespace quant::infra {
namespace {

// Test type that is Resettable
struct TestObject {
    int id = 0;
    double value = 0.0;
    std::string name;
    static std::atomic<int> constructed;
    static std::atomic<int> destructed;
    static std::atomic<int> reset_count;

    TestObject() { constructed.fetch_add(1); }
    ~TestObject() { destructed.fetch_add(1); }

    void reset() {
        id = 0;
        value = 0.0;
        name.clear();
        reset_count.fetch_add(1);
    }
};

std::atomic<int> TestObject::constructed{0};
std::atomic<int> TestObject::destructed{0};
std::atomic<int> TestObject::reset_count{0};

TEST(ObjectPoolTest, AcquireRelease) {
    TestObject::constructed = 0;
    TestObject::destructed = 0;
    TestObject::reset_count = 0;

    ObjectPool<TestObject> pool;
    {
        auto obj = pool.acquire();
        ASSERT_NE(obj, nullptr);
        obj->id = 42;
        obj->value = 3.14;
        obj->name = "test";
    }  // shared_ptr goes out of scope, returns to pool

    EXPECT_GE(TestObject::constructed, 1);
}

TEST(ObjectPoolTest, PoolReusesObject) {
    TestObject::constructed = 0;
    TestObject::reset_count = 0;

    ObjectPool<TestObject> pool(ObjectPoolConfig<TestObject>{
        .initial_capacity = 1,
        .grow_factor = 2,
        .max_capacity = 0,
        .thread_safe = true,
    });

    {
        auto obj = pool.acquire();
        obj->id = 100;
    }

    // Object should be reset and reused
    auto obj2 = pool.acquire();
    // reset() should have been called
    EXPECT_GE(TestObject::reset_count, 1);
}

TEST(ObjectPoolTest, BatchAcquire) {
    ObjectPool<TestObject> pool(ObjectPoolConfig<TestObject>{
        .initial_capacity = 10,
    });

    auto objects = pool.acquire_batch(5);
    EXPECT_EQ(objects.size(), 5);

    for (auto& obj : objects) {
        ASSERT_NE(obj, nullptr);
        obj->id = 1;
    }

    auto stats = pool.stats();
    EXPECT_GE(stats.acquire_count, 5);
}

TEST(ObjectPoolTest, Stats) {
    ObjectPool<TestObject> pool(ObjectPoolConfig<TestObject>{
        .initial_capacity = 10,
    });

    auto stats = pool.stats();
    EXPECT_GE(stats.total_allocated, 10);
    EXPECT_EQ(stats.total_in_use, 0);
    EXPECT_GE(stats.total_available, 0);
}

TEST(ObjectPoolTest, ConcurrentAcquire) {
    ObjectPool<TestObject> pool;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 25; ++i) {
                auto obj = pool.acquire();
                if (obj) {
                    obj->id = i;
                    success_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), 100);
}

TEST(ObjectPoolTest, Warmup) {
    ObjectPool<TestObject> pool;
    pool.warmup(50);
    auto stats = pool.stats();
    EXPECT_GE(stats.total_allocated, 50);
}

}  // namespace
}  // namespace quant::infra
