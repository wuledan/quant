// etcd_client.cc — Coroutine-friendly etcd v3 client (etcdctl backend)
//
// Uses etcdctl subprocess for reliability. Watch runs etcdctl watch
// in a background thread, parses output, and posts events to an
// UnboundedQueue for coroutine consumption.

#include "cpp/quant/infra/etcd_client.h"

#include <array>
#include <cstdio>
#include <sstream>
#include <thread>

namespace quant::infra {

// ── Constructor / Destructor ──

EtcdClient::EtcdClient(const std::string& endpoints,
                       const std::string& etcdctl_path)
    : endpoints_(endpoints)
    , etcdctl_path_(etcdctl_path) {
    // Check if etcdctl is available
    available_ = (run_etcdctl({"version"}) == 0);
}

EtcdClient::~EtcdClient() {
    cancel_watches();
}

// ── Subprocess helper ──

int EtcdClient::run_etcdctl(const std::vector<std::string>& args,
                             std::string* stdout_out,
                             std::string* stderr_out) {
    // Build command: etcdctl --endpoints=... --write-out=json args...
    std::string cmd = etcdctl_path_;
    cmd += " --endpoints=" + endpoints_;
    for (const auto& arg : args) {
        cmd += " '";
        // Simple escaping for single quotes in args
        std::string escaped = arg;
        // Replace ' with '\''
        size_t pos = 0;
        while ((pos = escaped.find('\'', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "'\\''");
            pos += 4;
        }
        cmd += escaped;
        cmd += "'";
    }
    cmd += " 2>&1";

    // Run and capture output
    std::string output;
    std::array<char, 4096> buffer;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return -1;
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }

    int exit_code = pclose(pipe);
    // pclose returns the shell's exit status; extract the real exit code
    int status = WEXITSTATUS(exit_code);

    if (stdout_out) {
        *stdout_out = output;
    }

    return status;
}

// ── Synchronous operations ──

std::optional<std::string> EtcdClient::get(const std::string& key) {
    std::string output;
    int rc = run_etcdctl({"get", key, "--print-value-only"}, &output);
    if (rc != 0) return std::nullopt;

    // Strip trailing newline
    while (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }

    if (output.empty()) return std::nullopt;
    return output;
}

bool EtcdClient::put(const std::string& key, const std::string& value) {
    return run_etcdctl({"put", key, value}) == 0;
}

bool EtcdClient::remove(const std::string& key) {
    return run_etcdctl({"del", key}) == 0;
}

std::vector<std::pair<std::string, std::string>>
EtcdClient::get_prefix(const std::string& prefix) {
    std::string output;
    int rc = run_etcdctl({"get", prefix, "--prefix"}, &output);
    if (rc != 0) return {};

    return parse_get_output(output);
}

// ── Parse etcdctl get output ──
// etcdctl get --prefix output format:
//   key1\n
//   value1\n
//   key2\n
//   value2\n

std::vector<std::pair<std::string, std::string>>
EtcdClient::parse_get_output(const std::string& output) {
    std::vector<std::pair<std::string, std::string>> result;
    std::istringstream stream(output);
    std::string line;

    // Read key-value pairs
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        std::string key = line;
        std::string value;
        if (std::getline(stream, value)) {
            result.emplace_back(std::move(key), std::move(value));
        }
    }

    return result;
}

// ── Watch ──

CoTask<void> EtcdClient::co_watch_prefix(
    const std::string& prefix,
    std::function<void(std::string key, std::string value, bool is_delete)> callback) {

    if (!available_) co_return;

    // Start etcdctl watch in a background thread
    cancelled_.store(false);

    std::thread watch_thread([this, prefix]() {
        // Build command: etcdctl watch --prefix {prefix}
        // Output format for each event:
        //   PUT\nkey\nvalue\n   or   DELETE\nkey\n\n
        std::string cmd = etcdctl_path_;
        cmd += " --endpoints=" + endpoints_;
        cmd += " watch --prefix '";
        cmd += prefix;
        cmd += "' 2>/dev/null";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return;

        std::array<char, 4096> buffer;
        std::string accumulated;

        while (!cancelled_.load(std::memory_order_relaxed)) {
            if (fgets(buffer.data(), buffer.size(), pipe) == nullptr) {
                break;
            }
            accumulated += buffer.data();

            // Parse events line-by-line from accumulated buffer.
            // After each successful parse, consume those lines.
            // If a partial event remains, keep it for next fgets.
            while (true) {
                auto nl1 = accumulated.find('\n');
                if (nl1 == std::string::npos) break;  // no complete line yet

                std::string event_type = accumulated.substr(0, nl1);
                accumulated.erase(0, nl1 + 1);

                if (event_type == "PUT") {
                    // Need 2 more lines: key and value
                    auto nl2 = accumulated.find('\n');
                    if (nl2 == std::string::npos) {
                        // Partial — push "PUT" back and wait
                        accumulated = "PUT\n" + accumulated;
                        break;
                    }
                    std::string key = accumulated.substr(0, nl2);
                    accumulated.erase(0, nl2 + 1);

                    auto nl3 = accumulated.find('\n');
                    if (nl3 == std::string::npos) {
                        // Partial — push "PUT\nkey" back and wait
                        accumulated = "PUT\n" + key + "\n" + accumulated;
                        break;
                    }
                    std::string value = accumulated.substr(0, nl3);
                    accumulated.erase(0, nl3 + 1);

                    WatchEvent evt;
                    evt.key = std::move(key);
                    evt.value = std::move(value);
                    evt.is_delete = false;
                    event_queue_.enqueue(std::move(evt));

                } else if (event_type == "DELETE") {
                    // Need 1 more line: key
                    auto nl2 = accumulated.find('\n');
                    if (nl2 == std::string::npos) {
                        // Partial — push "DELETE" back and wait
                        accumulated = "DELETE\n" + accumulated;
                        break;
                    }
                    std::string key = accumulated.substr(0, nl2);
                    accumulated.erase(0, nl2 + 1);

                    WatchEvent evt;
                    evt.key = std::move(key);
                    evt.is_delete = true;
                    event_queue_.enqueue(std::move(evt));

                }
                // else: skip unknown line types
            }
        }

        pclose(pipe);
    });

    watch_thread.detach();

    // Consume events from the queue and invoke the callback
    while (!cancelled_.load(std::memory_order_relaxed)) {
        auto evt = co_await event_queue_.dequeue();
        if (evt.key.empty()) break;
        callback(std::move(evt.key), std::move(evt.value), evt.is_delete);
    }
}

void EtcdClient::cancel_watches() {
    cancelled_.store(true);
    // Enqueue a poison pill to wake up the consumer coroutine
    WatchEvent poison;
    poison.key.clear();  // Empty key = shutdown signal
    poison.is_delete = false;
    event_queue_.enqueue(std::move(poison));
}

}  // namespace quant::infra
