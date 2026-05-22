// etcd_client_test.cc — Integration tests for EtcdClient
//
// These tests require an etcd instance running at 127.0.0.1:2379
// and the etcdctl binary available in PATH.
// If either is not available, tests are skipped with GTEST_SKIP().
#include "cpp/quant/infra/etcd_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace quant::infra {
namespace {

class EtcdClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        client_ = std::make_unique<EtcdClient>("http://127.0.0.1:2379");
        if (!client_->is_available()) {
            GTEST_SKIP() << "etcdctl or etcd not available at 127.0.0.1:2379";
        }
    }

    void TearDown() override {
        if (client_) {
            // Clean up test keys
            client_->remove("/test/key1");
            client_->remove("/test/key2");
            client_->remove("/test/prefix/a");
            client_->remove("/test/prefix/b");
            client_->remove("/test/prefix/c");
            client_->remove("/test/watch/testkey");
        }
    }

    std::unique_ptr<EtcdClient> client_;
};

TEST_F(EtcdClientTest, PutAndGet) {
    EXPECT_TRUE(client_->put("/test/key1", "hello"));
    auto val = client_->get("/test/key1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "hello");
}

TEST_F(EtcdClientTest, GetNonexistentKey) {
    auto val = client_->get("/test/nonexistent_key_xyz");
    EXPECT_FALSE(val.has_value());
}

TEST_F(EtcdClientTest, PutOverwrites) {
    EXPECT_TRUE(client_->put("/test/key1", "first"));
    EXPECT_TRUE(client_->put("/test/key1", "second"));
    auto val = client_->get("/test/key1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "second");
}

TEST_F(EtcdClientTest, Remove) {
    client_->put("/test/key1", "temp");
    EXPECT_TRUE(client_->remove("/test/key1"));
    auto val = client_->get("/test/key1");
    EXPECT_FALSE(val.has_value());
}

TEST_F(EtcdClientTest, GetPrefix) {
    client_->put("/test/prefix/a", "alpha");
    client_->put("/test/prefix/b", "beta");
    client_->put("/test/prefix/c", "gamma");

    auto entries = client_->get_prefix("/test/prefix/");
    EXPECT_GE(entries.size(), 3u);

    // Verify all expected keys are present
    bool found_a = false, found_b = false, found_c = false;
    for (auto& [key, value] : entries) {
        if (key == "/test/prefix/a") { found_a = true; EXPECT_EQ(value, "alpha"); }
        if (key == "/test/prefix/b") { found_b = true; EXPECT_EQ(value, "beta"); }
        if (key == "/test/prefix/c") { found_c = true; EXPECT_EQ(value, "gamma"); }
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);
    EXPECT_TRUE(found_c);
}

TEST_F(EtcdClientTest, WatchPrefix) {
    // Start a watch in a coroutine context
    std::atomic<int> event_count{0};
    std::string received_key;
    std::string received_value;
    std::atomic<bool> delete_received{false};
    std::atomic<bool> watch_done{false};

    // Put a key BEFORE starting the watch — this tests the "initial
    // state" case. The watch will only see changes AFTER it starts.
    // Then put another key AFTER the watch starts to test live events.

    // Start the watch in a thread with blockingWait
    std::thread watch_thread([&]() {
        blockingWait(client_->co_watch_prefix(
            "/test/watch/",
            [&](std::string key, std::string value, bool is_delete) {
                received_key = key;
                received_value = value;
                delete_received = is_delete;
                event_count.fetch_add(1);
                // Auto-cancel after first event to prevent hanging
                if (event_count.load() >= 1) {
                    client_->cancel_watches();
                    watch_done.store(true);
                }
            }));
    });

    // Give the watch time to start (etcdctl watch subprocess needs time)
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Put a key that should trigger the watch
    client_->put("/test/watch/testkey", "watch_value");

    // Wait for the event to be delivered (with timeout)
    for (int i = 0; i < 50; ++i) {
        if (watch_done.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cancel watches if not done yet
    if (!watch_done.load()) {
        client_->cancel_watches();
    }

    // Detach the watch thread (it will exit after cancel)
    watch_thread.detach();

    EXPECT_GE(event_count.load(), 1);
    if (event_count.load() > 0) {
        EXPECT_EQ(received_key, "/test/watch/testkey");
        EXPECT_EQ(received_value, "watch_value");
        EXPECT_FALSE(delete_received);
    }

    // Clean up
    client_->remove("/test/watch/testkey");
}

}  // namespace
}  // namespace quant::infra
