// http_server.h — Lightweight HTTP/1.1 server for REST API
//
// Provides a simple HTTP server that accepts connections, parses HTTP requests,
// routes them to a handler callback, and sends back HTTP responses.
// Designed to expose StrategyApi over HTTP so external clients (Python, curl,
// etc.) can register strategies, upload IR graphs, trigger backtests, etc.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace quant::network {

// ── HTTP Request ──
struct HttpRequest {
    std::string method;   // GET, POST, PUT, DELETE
    std::string path;     // e.g. /api/strategies
    std::string body;     // Request body (for POST/PUT)
    std::unordered_map<std::string, std::string> headers;
    std::string query_string;  // e.g. "page=1&size=10"
};

// ── HTTP Response ──
struct HttpResponse {
    int status_code = 200;
    std::string body;
    std::string content_type = "application/json";

    std::string serialize() const {
        const char* reason = "OK";
        switch (status_code) {
            case 200: reason = "OK"; break;
            case 201: reason = "Created"; break;
            case 204: reason = "No Content"; break;
            case 400: reason = "Bad Request"; break;
            case 404: reason = "Not Found"; break;
            case 405: reason = "Method Not Allowed"; break;
            case 500: reason = "Internal Server Error"; break;
        }

        std::string resp;
        resp.reserve(256 + body.size());
        resp += "HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n";
        resp += "Content-Type: " + content_type + "\r\n";
        resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        resp += "Access-Control-Allow-Origin: *\r\n";
        resp += "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
        resp += "Access-Control-Allow-Headers: Content-Type\r\n";
        resp += "Connection: close\r\n";
        resp += "\r\n";
        resp += body;
        return resp;
    }
};

// ── Request handler callback ──
using HttpRequestHandler = std::function<HttpResponse(const HttpRequest&)>;

// ── HTTP Server config ──
struct HttpServerConfig {
    int port = 9090;
    std::string host = "0.0.0.0";
    int backlog = 128;
    size_t max_request_size = 16 * 1024 * 1024;  // 16 MB
    int read_timeout_ms = 30000;
};

// ── HTTP Server ──
class HttpServer {
public:
    explicit HttpServer(HttpServerConfig config = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Set the request handler
    void set_handler(HttpRequestHandler handler) { handler_ = std::move(handler); }

    // Lifecycle
    bool start();
    void stop() noexcept;
    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

    // Stats
    struct Stats {
        uint64_t requests_received{0};
        uint64_t requests_served{0};
        uint64_t requests_errored{0};
        uint64_t bytes_received{0};
        uint64_t bytes_sent{0};
    };
    Stats stats() const noexcept;

    // Parse raw HTTP request string (public static, no instance state needed)
    static bool parse_request(const std::string& raw, HttpRequest& req);

private:
    void accept_loop();
    void handle_connection(int client_fd);

    HttpServerConfig config_;
    HttpRequestHandler handler_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    std::unique_ptr<std::thread> accept_thread_;

    struct AtomicStats {
        std::atomic<uint64_t> requests_received{0};
        std::atomic<uint64_t> requests_served{0};
        std::atomic<uint64_t> requests_errored{0};
        std::atomic<uint64_t> bytes_received{0};
        std::atomic<uint64_t> bytes_sent{0};
    };
    mutable AtomicStats stats_;
};

}  // namespace quant::network
