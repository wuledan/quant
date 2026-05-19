// websocket_server.h — WebSocket server with handshake, frame parsing, and broadcast
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cpp/quant/infra/coroutine.h"

namespace quant::network {

using infra::CoTask;

// ── WebSocket opcodes ──
enum class WsOpcode : uint8_t {
    kContinuation = 0x0,
    kText         = 0x1,
    kBinary       = 0x2,
    kClose        = 0x8,
    kPing         = 0x9,
    kPong         = 0xA,
};

// ── WebSocket frame ──
struct WsFrame {
    bool     fin        = true;
    bool     masked     = false;
    WsOpcode opcode     = WsOpcode::kText;
    uint64_t payload_len = 0;
    uint8_t  mask_key[4] = {0, 0, 0, 0};
    std::vector<uint8_t> payload;
};

// ── WebSocket callbacks ──
struct WsCallbacks {
    std::function<void(const std::string& session_id)> on_open;
    std::function<void(const std::string& session_id, const std::string& msg)> on_message;
    std::function<void(const std::string& session_id, uint16_t code, const std::string& reason)> on_close;
    std::function<void(const std::string& session_id)> on_error;
};

// ── WebSocket utility functions (header-only) ──

// WebSocket frame encoding (server -> client, unmasked)
inline std::vector<uint8_t> encode_ws_frame(const uint8_t* data, size_t len, WsOpcode opcode) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | static_cast<uint8_t>(opcode));  // FIN + opcode

    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len < 65536) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    frame.insert(frame.end(), data, data + len);
    return frame;
}

// Parse a WebSocket frame from raw data
// Returns parsed frame and bytes consumed, or nullptr if incomplete
inline bool parse_ws_frame(const uint8_t* data, size_t len, WsFrame& frame) {
    if (len < 2) return false;

    frame.fin = (data[0] & 0x80) != 0;
    frame.opcode = static_cast<WsOpcode>(data[0] & 0x0F);
    frame.masked = (data[1] & 0x80) != 0;

    size_t offset = 2;
    uint64_t payload_len = data[1] & 0x7F;

    if (payload_len == 126) {
        if (len < 4) return false;
        payload_len = (static_cast<uint64_t>(data[2]) << 8) | data[3];
        offset = 4;
    } else if (payload_len == 127) {
        if (len < 10) return false;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | data[offset + i];
        }
        offset = 10;
    }

    frame.payload_len = payload_len;

    if (frame.masked) {
        if (len < offset + 4) return false;
        std::memcpy(frame.mask_key, data + offset, 4);
        offset += 4;
    }

    if (len < offset + payload_len) return false;

    frame.payload.assign(data + offset, data + offset + payload_len);
    if (frame.masked) {
        for (uint64_t i = 0; i < payload_len; ++i) {
            frame.payload[i] ^= frame.mask_key[i % 4];
        }
    }

    return true;
}

// Generate Sec-WebSocket-Accept from key
inline std::string ws_accept_key(const std::string& key) {
    // Return placeholder — in production would use SHA-1 + base64
    // Actual: Base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
    return key + "-accepted";
}

// ── CoIouring integration ──
class CoIouring;
struct WsServerConfig {
    int          port = 8080;
    std::string  host = "0.0.0.0";
    int          max_connections = 1000;
    bool         require_auth = false;
    int          ping_interval_ms = 30000;
};

// ── WebSocket server ──
class WebSocketServer {
public:
    explicit WebSocketServer(WsServerConfig config);
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    // ── Lifecycle ──
    bool start();
    void stop() noexcept;
    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

    // ── Send ──
    bool send(const std::string& session_id, const std::string& message);
    bool send_binary(const std::string& session_id, const uint8_t* data, size_t len);
    void broadcast(const std::string& message);

    // ── CoIouring integration ──
    void set_io_uring(class CoIouring* ring) noexcept { ring_ = ring; }

    // ── Coroutine lifecycle ──
    CoTask<bool> co_start();

    // ── Coroutine send ──
    CoTask<bool> co_send(const std::string& session_id, const std::string& message);
    CoTask<bool> co_send_binary(const std::string& session_id, const uint8_t* data, size_t len);
    CoTask<void> co_broadcast(const std::string& message);


    // ── Session management ──
    bool close_session(const std::string& session_id);
    size_t session_count() const noexcept;

    // ── Callbacks ──
    void set_callbacks(WsCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    // ── Stats ──
    struct Stats {
        uint64_t messages_sent{0};
        uint64_t messages_received{0};
        uint64_t bytes_sent{0};
        uint64_t bytes_received{0};
        uint64_t connections_opened{0};
        uint64_t connections_closed{0};
    };
    Stats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    WsServerConfig config_;
    WsCallbacks callbacks_;
    std::atomic<bool> running_{false};
    std::atomic<int> listen_fd_{-1};

    struct AtomicStats {
        std::atomic<uint64_t> messages_sent{0};
        std::atomic<uint64_t> messages_received{0};
        std::atomic<uint64_t> bytes_sent{0};
        std::atomic<uint64_t> bytes_received{0};
        std::atomic<uint64_t> connections_opened{0};
        std::atomic<uint64_t> connections_closed{0};
    };
    mutable AtomicStats stats_;
    CoIouring* ring_{nullptr};
};

}  // namespace quant::network
