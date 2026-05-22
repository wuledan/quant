// etcd_client.h — Coroutine-friendly etcd v3 client
//
// Uses etcdctl subprocess for reliability (etcd-cpp-apiv3 has gRPC
// channel issues in our build environment). The subprocess approach
// is simple, reliable, and suitable for our low-frequency etcd
// operations (strategy config, not hot-path data).
//
// Watch operations run etcdctl watch in a background thread and
// deliver events via folly::coro::UnboundedQueue for coroutine
// consumption.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cpp/quant/infra/coroutine.h"

namespace quant::infra {

class EtcdClient {
public:
    struct WatchEvent {
        std::string key;
        std::string value;   // empty for DELETE events
        bool is_delete;
    };

    // etcdctl_path: path to etcdctl binary (default: "etcdctl")
    // endpoints: etcd server endpoints (default: "http://127.0.0.1:2379")
    explicit EtcdClient(const std::string& endpoints = "http://127.0.0.1:2379",
                        const std::string& etcdctl_path = "etcdctl");
    ~EtcdClient();

    EtcdClient(const EtcdClient&) = delete;
    EtcdClient& operator=(const EtcdClient&) = delete;

    // ── Synchronous operations ──
    // These are blocking (subprocess call). Safe to call from any thread.

    std::optional<std::string> get(const std::string& key);
    bool put(const std::string& key, const std::string& value);
    bool remove(const std::string& key);

    // Get all key-value pairs with the given prefix
    std::vector<std::pair<std::string, std::string>> get_prefix(
        const std::string& prefix);

    // ── Watch (coroutine-friendly) ──
    // Watches a prefix for changes. etcdctl watch runs in a background
    // thread; events are posted to an UnboundedQueue and delivered to
    // the callback from a coroutine context.
    CoTask<void> co_watch_prefix(
        const std::string& prefix,
        std::function<void(std::string key, std::string value, bool is_delete)> callback);

    // Cancel all active watches (called automatically in destructor)
    void cancel_watches();

    // ── Health check ──
    // Returns true if etcdctl is available and etcd is reachable
    bool is_available() const noexcept { return available_; }

private:
    // Helper: run etcdctl command and capture stdout
    int run_etcdctl(const std::vector<std::string>& args,
                    std::string* stdout_out = nullptr,
                    std::string* stderr_out = nullptr);

    // Helper: parse etcdctl get output (key\nvalue\n pairs)
    static std::vector<std::pair<std::string, std::string>> parse_get_output(
        const std::string& output);

    std::string endpoints_;
    std::string etcdctl_path_;
    bool available_{false};

    // Watch state
    folly::coro::UnboundedQueue<WatchEvent, true, true> event_queue_;
    std::atomic<bool> cancelled_{false};
};

}  // namespace quant::infra
