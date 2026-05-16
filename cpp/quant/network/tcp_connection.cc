// tcp_connection.cc — TCP connection implementation using POSIX sockets
#include "cpp/quant/network/tcp_connection.h"

#include <cstring>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace quant::network {

// ── IoBuffer ──
void IoBuffer::grow(size_t min_capacity) {
    size_t new_cap = buffer_.size() * 2;
    while (new_cap < min_capacity) new_cap *= 2;
    std::vector<char> new_buf(new_cap);
    size_t readable = this->readable();
    if (readable > 0) {
        std::memcpy(new_buf.data(), buffer_.data() + read_pos_, readable);
    }
    buffer_ = std::move(new_buf);
    write_pos_ = readable;
    read_pos_ = 0;
}

// ── TcpConnection ──
TcpConnection::TcpConnection(TcpConfig config)
    : config_(std::move(config))
    , read_buf_(config_.read_buffer_size)
    , write_buf_(config_.write_buffer_size) {}

TcpConnection::~TcpConnection() {
    close_socket();
}

bool TcpConnection::create_socket() {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd_ < 0) return false;

    int opt = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    return true;
}

void TcpConnection::close_socket() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool TcpConnection::connect() {
    if (is_connected()) return true;

    set_state(TcpState::kConnecting);
    if (!create_socket()) {
        set_state(TcpState::kError);
        if (callbacks_.on_error) callbacks_.on_error("Failed to create socket");
        return false;
    }

    struct hostent* server = ::gethostbyname(config_.host.c_str());
    if (!server) {
        set_state(TcpState::kError);
        close_socket();
        return false;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    std::memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));

    int rc = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        set_state(TcpState::kError);
        close_socket();
        return false;
    }

    // Wait for connection using poll
    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLOUT;
    int ret = ::poll(&pfd, 1, config_.timeout_ms);

    if (ret <= 0) {
        set_state(TcpState::kError);
        close_socket();
        return false;
    }

    int err = 0;
    socklen_t err_len = sizeof(err);
    ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &err_len);
    if (err != 0) {
        set_state(TcpState::kError);
        close_socket();
        return false;
    }

    set_state(TcpState::kConnected);
    reconnect_attempts_ = 0;
    last_activity_ = infra::Timestamp::now();

    if (callbacks_.on_connected) callbacks_.on_connected();
    return true;
}

void TcpConnection::disconnect() noexcept {
    set_state(TcpState::kDisconnected);
    close_socket();
    read_buf_.clear();
    write_buf_.clear();
    if (callbacks_.on_disconnected) callbacks_.on_disconnected("manual disconnect");
}

bool TcpConnection::reconnect() {
    close_socket();
    if (config_.max_reconnect_attempts > 0 &&
        reconnect_attempts_ >= config_.max_reconnect_attempts) {
        set_state(TcpState::kError);
        return false;
    }
    set_state(TcpState::kReconnecting);
    ++reconnect_attempts_;
    return connect();
}

bool TcpConnection::send(const char* data, size_t len) {
    if (!is_connected()) return false;

    if (write_buf_.writable() < len) {
        write_buf_.grow(write_buf_.capacity() + len);
    }
    std::memcpy(write_buf_.write_data(), data, len);
    write_buf_.produced(len);

    // Try to flush immediately
    while (write_buf_.readable() > 0) {
        auto n = ::send(fd_, write_buf_.read_data(), write_buf_.readable(), MSG_DONTWAIT);
        if (n > 0) {
            write_buf_.consume(static_cast<size_t>(n));
        } else {
            break;
        }
    }
    return true;
}

bool TcpConnection::send(const std::string& data) {
    return send(data.data(), data.size());
}

size_t TcpConnection::recv(char* buf, size_t len) {
    if (!is_connected()) return 0;
    auto n = ::recv(fd_, buf, len, MSG_DONTWAIT);
    if (n > 0) {
        last_activity_ = infra::Timestamp::now();
    } else if (n == 0) {
        // Connection closed by peer
        set_state(TcpState::kDisconnected);
        if (callbacks_.on_disconnected) {
            callbacks_.on_disconnected("peer closed connection");
        }
    }
    return n > 0 ? static_cast<size_t>(n) : 0;
}

}  // namespace quant::network
