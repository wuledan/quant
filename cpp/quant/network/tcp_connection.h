// tcp_connection.h — Async TCP connection with reconnect support
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cpp/quant/infra/time_utils.h"

namespace quant::network {

// ── Connection state ──
enum class TcpState : uint8_t {
    kDisconnected = 0,
    kConnecting   = 1,
    kConnected    = 2,
    kReconnecting = 3,
    kError        = 4,
};

// ── IO buffer ──
class IoBuffer {
public:
    explicit IoBuffer(size_t capacity = 65536)
        : buffer_(capacity), read_pos_(0), write_pos_(0) {}

    size_t readable() const noexcept { return write_pos_ - read_pos_; }
    size_t writable() const noexcept { return buffer_.size() - write_pos_; }
    size_t capacity() const noexcept { return buffer_.size(); }

    const char* read_data() const noexcept { return buffer_.data() + read_pos_; }
    char* write_data() noexcept { return buffer_.data() + write_pos_; }

    void consume(size_t bytes) noexcept {
        read_pos_ += bytes;
        if (read_pos_ == write_pos_) {
            read_pos_ = write_pos_ = 0;
        }
    }

    void produced(size_t bytes) noexcept { write_pos_ += bytes; }

    void clear() noexcept { read_pos_ = write_pos_ = 0; }

    void grow(size_t min_capacity);

private:
    std::vector<char> buffer_;
    size_t read_pos_;
    size_t write_pos_;
};

// ── TCP connection config ──
struct TcpConfig {
    std::string  host;
    int          port = 0;
    bool         auto_reconnect = true;
    int          reconnect_delay_ms = 1000;
    int          max_reconnect_attempts = 10;
    size_t       read_buffer_size = 65536;
    size_t       write_buffer_size = 65536;
    int          timeout_ms = 30000;
};

// ── TCP connection callbacks ──
struct TcpCallbacks {
    std::function<void()>                      on_connected;
    std::function<void(const std::string&)>    on_disconnected;
    std::function<void(const char*, size_t)>   on_data;
    std::function<void(const std::string&)>    on_error;
};

// ── TCP connection ──
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    explicit TcpConnection(TcpConfig config);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // ── Lifecycle ──
    bool connect();
    void disconnect() noexcept;
    bool reconnect();

    // ── IO ──
    bool send(const char* data, size_t len);
    bool send(const std::string& data);
    size_t recv(char* buf, size_t len);

    // ── State ──
    TcpState state() const noexcept { return state_.load(std::memory_order_relaxed); }
    bool is_connected() const noexcept {
        return state_.load(std::memory_order_relaxed) == TcpState::kConnected;
    }
    const TcpConfig& config() const noexcept { return config_; }

    // ── Callbacks ──
    void set_callbacks(TcpCallbacks callbacks) { callbacks_ = std::move(callbacks); }
    void set_on_data(std::function<void(const char*, size_t)> cb) { callbacks_.on_data = std::move(cb); }

    // ── Read/write buffers ──
    IoBuffer& read_buffer() noexcept { return read_buf_; }
    IoBuffer& write_buffer() noexcept { return write_buf_; }

    // ── Socket handle (for integration with poll/select) ──
    int native_handle() const noexcept { return fd_; }

private:
    void set_state(TcpState s) noexcept { state_.store(s, std::memory_order_relaxed); }
    bool create_socket();
    void close_socket() noexcept;

    TcpConfig config_;
    TcpCallbacks callbacks_;
    std::atomic<TcpState> state_{TcpState::kDisconnected};
    int fd_ = -1;
    IoBuffer read_buf_;
    IoBuffer write_buf_;
    int reconnect_attempts_ = 0;
    infra::Timestamp last_activity_;
};

}  // namespace quant::network
