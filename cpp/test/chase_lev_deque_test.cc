#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "cpp/quant/infra/chase_lev_deque.h"

using quant::infra::ChaseLevDeque;

TEST(ChaseLevDeque, SinglePushPop) {
    ChaseLevDeque<int> dq;
    dq.push(42);
    auto val = dq.pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
}

TEST(ChaseLevDeque, LifoPop) {
    ChaseLevDeque<int> dq;
    dq.push(1);
    dq.push(2);
    dq.push(3);

    auto v1 = dq.pop();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 3);

    auto v2 = dq.pop();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 2);

    auto v3 = dq.pop();
    ASSERT_TRUE(v3.has_value());
    EXPECT_EQ(*v3, 1);
}

TEST(ChaseLevDeque, FifoSteal) {
    ChaseLevDeque<int> dq;
    dq.push(10);
    dq.push(20);
    dq.push(30);

    auto v = dq.steal();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 10);
}

TEST(ChaseLevDeque, EmptyPopReturnsNullopt) {
    ChaseLevDeque<int> dq;
    auto val = dq.pop();
    EXPECT_FALSE(val.has_value());
}

TEST(ChaseLevDeque, EmptyStealReturnsNullopt) {
    ChaseLevDeque<int> dq;
    auto val = dq.steal();
    EXPECT_FALSE(val.has_value());
}

TEST(ChaseLevDeque, DynamicResize) {
    ChaseLevDeque<int> dq(4);  // small initial capacity
    constexpr int N = 512;
    for (int i = 0; i < N; ++i) {
        dq.push(i);
    }

    std::set<int> retrieved;
    for (int i = 0; i < N; ++i) {
        auto v = dq.pop();
        ASSERT_TRUE(v.has_value()) << "failed at iteration " << i;
        retrieved.insert(*v);
    }

    EXPECT_EQ(retrieved.size(), static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(retrieved.count(i)) << "missing value " << i;
    }
}

TEST(ChaseLevDeque, ConcurrentSteal) {
    ChaseLevDeque<int> dq;
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        dq.push(i);
    }

    constexpr int kThieves = 4;
    std::vector<std::set<int>> stolen_per_thread(kThieves);
    std::vector<std::thread> threads;

    for (int t = 0; t < kThieves; ++t) {
        threads.emplace_back([&dq, &stolen_per_thread, t]() {
            while (true) {
                auto v = dq.steal();
                if (!v.has_value()) break;
                stolen_per_thread[t].insert(*v);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // Collect remaining items via pop
    std::set<int> remaining;
    while (true) {
        auto v = dq.pop();
        if (!v.has_value()) break;
        remaining.insert(*v);
    }

    // All stolen items are unique
    std::set<int> all_stolen;
    for (int t = 0; t < kThieves; ++t) {
        for (int v : stolen_per_thread[t]) {
            EXPECT_FALSE(all_stolen.count(v)) << "duplicate stolen item " << v;
            all_stolen.insert(v);
        }
    }

    // Total = stolen + remaining = N
    EXPECT_EQ(all_stolen.size() + remaining.size(), static_cast<size_t>(N));
}

TEST(ChaseLevDeque, StressPushPopSteal) {
    ChaseLevDeque<int> dq;
    constexpr int N = 10000;

    // Owner pushes all items
    for (int i = 0; i < N; ++i) {
        dq.push(i);
    }

    std::set<int> popped;
    std::set<int> stolen;
    std::mutex popped_mtx;

    // Owner pops in a loop
    std::atomic<bool> done{false};
    std::thread owner([&]() {
        while (!done.load(std::memory_order_relaxed)) {
            auto v = dq.pop();
            if (v.has_value()) {
                std::lock_guard<std::mutex> lk(popped_mtx);
                popped.insert(*v);
            }
        }
        // Final drain
        while (true) {
            auto v = dq.pop();
            if (!v.has_value()) break;
            std::lock_guard<std::mutex> lk(popped_mtx);
            popped.insert(*v);
        }
    });

    constexpr int kThieves = 4;
    std::vector<std::set<int>> stolen_per_thread(kThieves);
    std::vector<std::thread> thieves;
    for (int t = 0; t < kThieves; ++t) {
        thieves.emplace_back([&dq, &stolen_per_thread, t, &done]() {
            while (!done.load(std::memory_order_relaxed)) {
                auto v = dq.steal();
                if (v.has_value()) {
                    stolen_per_thread[t].insert(*v);
                }
            }
            // Final drain
            while (true) {
                auto v = dq.steal();
                if (!v.has_value()) break;
                stolen_per_thread[t].insert(*v);
            }
        });
    }

    // Signal done after a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    done.store(true);

    owner.join();
    for (auto& th : thieves) {
        th.join();
    }

    // Merge results
    std::set<int> all_stolen;
    for (int t = 0; t < kThieves; ++t) {
        for (int v : stolen_per_thread[t]) {
            EXPECT_FALSE(all_stolen.count(v)) << "duplicate stolen " << v;
            all_stolen.insert(v);
        }
    }

    // No overlap between popped and stolen
    for (int v : popped) {
        EXPECT_FALSE(all_stolen.count(v)) << "item in both popped and stolen: " << v;
    }

    // Total items = N, no lost items
    EXPECT_EQ(popped.size() + all_stolen.size(), static_cast<size_t>(N));
}