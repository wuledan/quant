// websocket_server.cc — WebSocket server implementation
#include "cpp/quant/network/websocket_server.h"
#include "cpp/quant/network/co_io.h"
#include "cpp/quant/network/tcp_connection.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace quant::network {

struct WebSocketServer::Impl {
    std::map<int, std::string> fd_to_session;    // socket fd -> session id
    std::map<std::string, int> session_to_fd;    // session id -> socket fd
    std::map<std::string, IoBuffer> session_buffers;
    mutable infra::AffinityMutex mutex;
    std::thread accept_thread;
    std::atomic<bool> stop{false};
};

WebSocketServer::WebSocketServer(WsServerConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {}

WebSocketServer::~WebSocketServer() { stop(); }

bool WebSocketServer::start() {
    if (running_.exchange(true)) return false;

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { running_.store(false); return false; }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); running_.store(false); return false;
    }
    if (::listen(fd, 128) < 0) {
        ::close(fd); running_.store(false); return false;
    }

    listen_fd_.store(fd, std::memory_order_relaxed);
    return true;
}

void WebSocketServer::stop() noexcept {
    running_.store(false, std::memory_order_relaxed);
    int fd = listen_fd_.exchange(-1, std::memory_order_relaxed);
    if (fd >= 0) ::close(fd);
}

bool WebSocketServer::send(const std::string& session_id, const std::string& message) {
    auto frame = encode_ws_frame(
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size(), WsOpcode::kText);
    stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_sent.fetch_add(frame.size(), std::memory_order_relaxed);
    return true;
}

bool WebSocketServer::send_binary(const std::string& session_id, const uint8_t* data, size_t len) {
    auto frame = encode_ws_frame(data, len, WsOpcode::kBinary);
    stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_sent.fetch_add(frame.size(), std::memory_order_relaxed);
    return true;
}

void WebSocketServer::broadcast(const std::string& message) {
    auto frame = encode_ws_frame(
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size(), WsOpcode::kText);
    stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_sent.fetch_add(frame.size() * session_count(), std::memory_order_relaxed);
}

bool WebSocketServer::close_session(const std::string& session_id) {
    auto lock = infra::blockingWait(impl_->mutex.co_scoped_lock());
    auto it = impl_->session_to_fd.find(session_id);
    if (it != impl_->session_to_fd.end()) {
        ::close(it->second);
        impl_->session_to_fd.erase(it);
        impl_->fd_to_session.erase(it->second);
        impl_->session_buffers.erase(session_id);
        stats_.connections_closed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

size_t WebSocketServer::session_count() const noexcept {
    auto lock = infra::blockingWait(impl_->mutex.co_scoped_lock());
    return impl_->session_to_fd.size();
}

WebSocketServer::Stats WebSocketServer::stats() const noexcept {
    Stats s;
    s.messages_sent = stats_.messages_sent.load(std::memory_order_relaxed);
    s.messages_received = stats_.messages_received.load(std::memory_order_relaxed);
    s.bytes_sent = stats_.bytes_sent.load(std::memory_order_relaxed);
    s.bytes_received = stats_.bytes_received.load(std::memory_order_relaxed);
    s.connections_opened = stats_.connections_opened.load(std::memory_order_relaxed);
    s.connections_closed = stats_.connections_closed.load(std::memory_order_relaxed);
    return s;
}

// ── Coroutine I/O (requires CoIouring) ──

CoTask<bool> WebSocketServer::co_start() {
    if (ring_ == nullptr) co_return false;

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) co_return false;

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); co_return false;
    }
    if (::listen(fd, 128) < 0) {
        ::close(fd); co_return false;
    }

    listen_fd_.store(fd, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    co_return true;
}

CoTask<bool> WebSocketServer::co_send(const std::string& session_id, const std::string& message) {
    auto frame = encode_ws_frame(
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size(), WsOpcode::kText);
    stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_sent.fetch_add(frame.size(), std::memory_order_relaxed);

    // Extract target fd under lock, then send outside lock to avoid
    // holding std::mutex across co_await (undefined behavior).
    int target_fd = -1;
    if (ring_ != nullptr) {
        auto lock = infra::blockingWait(impl_->mutex.co_scoped_lock());
        auto it = impl_->session_to_fd.find(session_id);
        if (it != impl_->session_to_fd.end()) {
            target_fd = it->second;
        }
    }
    if (target_fd >= 0) {
        ssize_t n = co_await ring_->co_send(target_fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        co_return n > 0;
    }
    co_return true;
}

CoTask<bool> WebSocketServer::co_send_binary(const std::string& session_id, const uint8_t* data, size_t len) {
    auto frame = encode_ws_frame(data, len, WsOpcode::kBinary);
    stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_sent.fetch_add(frame.size(), std::memory_order_relaxed);

    // Extract target fd under lock, then send outside lock.
    int target_fd = -1;
    if (ring_ != nullptr) {
        auto lock = infra::blockingWait(impl_->mutex.co_scoped_lock());
        auto it = impl_->session_to_fd.find(session_id);
        if (it != impl_->session_to_fd.end()) {
            target_fd = it->second;
        }
    }
    if (target_fd >= 0) {
        ssize_t n = co_await ring_->co_send(target_fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        co_return n > 0;
    }
    co_return true;
}

CoTask<void> WebSocketServer::co_broadcast(const std::string& message) {
    auto frame = encode_ws_frame(
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size(), WsOpcode::kText);
    stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_sent.fetch_add(frame.size() * session_count(), std::memory_order_relaxed);

    if (ring_ != nullptr) {
        // Collect all target fds under lock, then send outside lock.
        std::vector<int> targets;
        {
            auto lock = infra::blockingWait(impl_->mutex.co_scoped_lock());
            targets.reserve(impl_->fd_to_session.size());
            for (const auto& [fid, sid] : impl_->fd_to_session) {
                targets.push_back(fid);
            }
        }
        for (int target_fd : targets) {
            co_await ring_->co_send(target_fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        }
    }
}

}  // namespace quant::network
