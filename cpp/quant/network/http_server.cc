// http_server.cc — Lightweight HTTP/1.1 server implementation
//
// Accepts TCP connections, reads HTTP requests, routes to handler,
// and sends HTTP responses. Uses blocking I/O on a dedicated thread
// per connection (sufficient for internal API traffic).

#include "cpp/quant/network/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace quant::network {

// ────────────────────────────────────────────────────────────────
// Construction / Destruction
// ────────────────────────────────────────────────────────────────

HttpServer::HttpServer(HttpServerConfig config)
    : config_(std::move(config)) {}

HttpServer::~HttpServer() {
    stop();
}

// ────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────

bool HttpServer::start() {
    if (running_.load(std::memory_order_relaxed)) return true;

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "[HttpServer] socket() failed: " << std::strerror(errno) << "\n";
        return false;
    }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));
    if (config_.host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        ::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr);
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[HttpServer] bind() failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, config_.backlog) < 0) {
        std::cerr << "[HttpServer] listen() failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true, std::memory_order_release);

    accept_thread_ = std::make_unique<std::thread>([this]() { accept_loop(); });

    std::cout << "[HttpServer] Listening on " << config_.host << ":"
              << config_.port << "\n";
    return true;
}

void HttpServer::stop() noexcept {
    if (!running_.load(std::memory_order_relaxed)) return;

    running_.store(false, std::memory_order_release);

    if (listen_fd_ >= 0) {
        // shutdown() wakes up any thread blocked on accept() — close() alone
        // does not reliably cause accept() to return on all Linux kernels.
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (accept_thread_ && accept_thread_->joinable()) {
        accept_thread_->join();
    }
    accept_thread_.reset();
}

// ────────────────────────────────────────────────────────────────
// Accept loop
// ────────────────────────────────────────────────────────────────

void HttpServer::accept_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_,
                                  reinterpret_cast<struct sockaddr*>(&client_addr),
                                  &client_len);
        if (client_fd < 0) {
            if (!running_.load(std::memory_order_relaxed)) break;
            continue;
        }

        // Handle each connection in a detached thread
        std::thread([this, client_fd]() {
            handle_connection(client_fd);
        }).detach();
    }
}

// ────────────────────────────────────────────────────────────────
// Handle a single HTTP connection
// ────────────────────────────────────────────────────────────────

void HttpServer::handle_connection(int client_fd) {
    // Read the full request
    std::string raw;
    raw.resize(4096);
    size_t total_read = 0;

    // Read headers first
    constexpr std::string_view kHeaderEnd = "\r\n\r\n";
    size_t header_end_pos = std::string::npos;

    while (running_.load(std::memory_order_relaxed)) {
        if (total_read >= raw.size()) {
            raw.resize(raw.size() * 2);
        }

        ssize_t n = ::recv(client_fd, raw.data() + total_read,
                           raw.size() - total_read, 0);
        if (n <= 0) {
            ::close(client_fd);
            return;
        }

        total_read += static_cast<size_t>(n);
        stats_.bytes_received.fetch_add(static_cast<uint64_t>(n),
                                         std::memory_order_relaxed);

        // Check if we have complete headers (only search within received data)
        raw.resize(total_read);
        header_end_pos = raw.find(kHeaderEnd);
        if (header_end_pos != std::string::npos) break;

        if (total_read >= config_.max_request_size) {
            // Request too large
            std::string resp = HttpResponse{413, "Request too large"}.serialize();
            ::send(client_fd, resp.data(), resp.size(), 0);
            ::close(client_fd);
            return;
        }
    }

    // Parse Content-Length to read body
    size_t body_start = header_end_pos + kHeaderEnd.size();
    size_t body_received = total_read - body_start;
    size_t content_length = 0;

    // Find Content-Length header
    std::string_view header_section(raw.data(), header_end_pos);
    auto cl_pos = header_section.find("Content-Length:");
    if (cl_pos == std::string_view::npos) {
        cl_pos = header_section.find("content-length:");
    }
    if (cl_pos != std::string_view::npos) {
        auto val_start = cl_pos + 15;  // strlen("Content-Length:")
        while (val_start < header_section.size() && header_section[val_start] == ' ') {
            val_start++;
        }
        content_length = 0;
        while (val_start < header_section.size() &&
               header_section[val_start] >= '0' && header_section[val_start] <= '9') {
            content_length = content_length * 10 + (header_section[val_start] - '0');
            val_start++;
        }
    }

    // Read remaining body if needed
    while (body_received < content_length && running_.load(std::memory_order_relaxed)) {
        if (total_read >= raw.size()) {
            raw.resize(raw.size() * 2);
        }
        ssize_t n = ::recv(client_fd, raw.data() + total_read,
                           raw.size() - total_read, 0);
        if (n <= 0) break;
        total_read += static_cast<size_t>(n);
        body_received = total_read - body_start;
        stats_.bytes_received.fetch_add(static_cast<uint64_t>(n),
                                         std::memory_order_relaxed);
    }

    stats_.requests_received.fetch_add(1, std::memory_order_relaxed);

    // Parse the request
    HttpRequest req;
    std::string raw_str(raw.data(), total_read);
    if (!parse_request(raw_str, req)) {
        std::string resp = HttpResponse{400, "Bad Request"}.serialize();
        ::send(client_fd, resp.data(), resp.size(), 0);
        stats_.requests_errored.fetch_add(1, std::memory_order_relaxed);
        ::close(client_fd);
        return;
    }

    // Handle CORS preflight
    if (req.method == "OPTIONS") {
        HttpResponse resp;
        resp.status_code = 204;
        resp.body = "";
        std::string serialized = resp.serialize();
        ::send(client_fd, serialized.data(), serialized.size(), 0);
        stats_.requests_served.fetch_add(1, std::memory_order_relaxed);
        stats_.bytes_sent.fetch_add(serialized.size(), std::memory_order_relaxed);
        ::close(client_fd);
        return;
    }

    // Dispatch to handler
    HttpResponse http_resp;
    if (handler_) {
        http_resp = handler_(req);
    } else {
        http_resp.status_code = 500;
        http_resp.body = R"({"error":"no handler configured"})";
    }

    std::string serialized = http_resp.serialize();
    ::send(client_fd, serialized.data(), serialized.size(), 0);
    stats_.requests_served.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_sent.fetch_add(serialized.size(), std::memory_order_relaxed);
    ::close(client_fd);
}

// ────────────────────────────────────────────────────────────────
// HTTP request parser
// ────────────────────────────────────────────────────────────────

bool HttpServer::parse_request(const std::string& raw, HttpRequest& req) {
    // Find end of first line
    auto first_line_end = raw.find("\r\n");
    if (first_line_end == std::string::npos) return false;

    std::string_view first_line(raw.data(), first_line_end);

    // Parse: METHOD PATH HTTP/1.x
    auto sp1 = first_line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    auto sp2 = first_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;

    req.method = std::string(first_line.substr(0, sp1));
    std::string_view full_path = first_line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Split path and query string
    auto qmark = full_path.find('?');
    if (qmark != std::string_view::npos) {
        req.path = std::string(full_path.substr(0, qmark));
        req.query_string = std::string(full_path.substr(qmark + 1));
    } else {
        req.path = std::string(full_path);
    }

    // Parse headers
    auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    size_t pos = first_line_end + 2;  // skip first line + \r\n
    while (pos < header_end) {
        auto line_end = raw.find("\r\n", pos);
        if (line_end == std::string::npos || line_end > header_end) break;

        std::string_view line(raw.data() + pos, line_end - pos);
        auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string key(line.substr(0, colon));
            std::string value(line.substr(colon + 1));
            // Trim leading whitespace from value
            size_t start = 0;
            while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
                start++;
            }
            req.headers[key] = value.substr(start);
        }

        pos = line_end + 2;
    }

    // Extract body
    size_t body_start = header_end + 4;
    if (body_start < raw.size()) {
        req.body = raw.substr(body_start);
    }

    return true;
}

// ────────────────────────────────────────────────────────────────
// Stats
// ────────────────────────────────────────────────────────────────

HttpServer::Stats HttpServer::stats() const noexcept {
    return Stats{
        .requests_received = stats_.requests_received.load(std::memory_order_relaxed),
        .requests_served = stats_.requests_served.load(std::memory_order_relaxed),
        .requests_errored = stats_.requests_errored.load(std::memory_order_relaxed),
        .bytes_received = stats_.bytes_received.load(std::memory_order_relaxed),
        .bytes_sent = stats_.bytes_sent.load(std::memory_order_relaxed),
    };
}

}  // namespace quant::network
