// http_server_test.cc — HTTP server unit and integration tests
//
// Tests cover:
//   1. ParseValidGetRequest   — parse_request on GET with query string
//   2. ParsePostWithBody      — parse_request on POST with JSON body
//   3. ParseMalformedRequest  — parse_request rejects garbage input
//   4. HttpResponseSerialization — serialize() output format
//   5. CorsPreflightResponse  — OPTIONS → 204 + CORS headers
//   6. HttpServerStartStop    — start/stop lifecycle, idempotent
//   7. HttpServerRouting      — real TCP: handler called, correct response
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cpp/quant/network/http_server.h"

namespace net = quant::network;

// ── Helper: send raw HTTP over TCP, return full response ──
static std::string send_http(int port, const std::string& raw_request) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // 2-second connect + recv timeout
    struct timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return "";
    }

    ::send(fd, raw_request.data(), raw_request.size(), 0);

    std::string resp;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return resp;
}

// ─────────────────────────────────────────────────────────────
// Test 1: ParseValidGetRequest
// ─────────────────────────────────────────────────────────────
TEST(HttpParserTest, ParseValidGetRequest) {
    std::string raw =
        "GET /api/strategies?page=1&size=10 HTTP/1.1\r\n"
        "Host: localhost:9090\r\n"
        "Accept: application/json\r\n"
        "\r\n";

    net::HttpRequest req;
    ASSERT_TRUE(net::HttpServer::parse_request(raw, req));

    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.path, "/api/strategies");
    EXPECT_EQ(req.query_string, "page=1&size=10");
    EXPECT_TRUE(req.body.empty());

    auto it = req.headers.find("Host");
    ASSERT_NE(it, req.headers.end());
    EXPECT_EQ(it->second, "localhost:9090");

    it = req.headers.find("Accept");
    ASSERT_NE(it, req.headers.end());
    EXPECT_EQ(it->second, "application/json");
}

// ─────────────────────────────────────────────────────────────
// Test 2: ParsePostWithBody
// ─────────────────────────────────────────────────────────────
TEST(HttpParserTest, ParsePostWithBody) {
    std::string body = R"({"name":"test","value":42})";
    std::string raw =
        "POST /api/strategies HTTP/1.1\r\n"
        "Host: localhost:9090\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    net::HttpRequest req;
    ASSERT_TRUE(net::HttpServer::parse_request(raw, req));

    EXPECT_EQ(req.method, "POST");
    EXPECT_EQ(req.path, "/api/strategies");
    EXPECT_EQ(req.body, body);

    auto it = req.headers.find("Content-Type");
    ASSERT_NE(it, req.headers.end());
    EXPECT_EQ(it->second, "application/json");
}

// ─────────────────────────────────────────────────────────────
// Test 3: ParseMalformedRequest
// ─────────────────────────────────────────────────────────────
TEST(HttpParserTest, ParseMalformedRequest) {
    net::HttpRequest req;

    // Empty
    EXPECT_FALSE(net::HttpServer::parse_request("", req));

    // No CRLF
    EXPECT_FALSE(net::HttpServer::parse_request("GET /index.html", req));

    // Only one token on request line
    EXPECT_FALSE(net::HttpServer::parse_request("GARBAGE\r\n", req));

    // Missing header terminator
    EXPECT_FALSE(net::HttpServer::parse_request(
        "GET / HTTP/1.1\r\nHost: localhost\r\n", req));
}

// ─────────────────────────────────────────────────────────────
// Test 4: HttpResponseSerialization
// ─────────────────────────────────────────────────────────────
TEST(HttpResponseTest, Serialization) {
    net::HttpResponse resp;
    resp.status_code = 200;
    resp.body = R"({"status":"ok"})";
    resp.content_type = "application/json";

    std::string s = resp.serialize();

    // Status line
    EXPECT_EQ(s.find("HTTP/1.1 200 OK"), 0u);

    // Headers
    EXPECT_NE(s.find("Content-Type: application/json"), std::string::npos);
    EXPECT_NE(s.find("Content-Length: 15"), std::string::npos);
    EXPECT_NE(s.find("Access-Control-Allow-Origin: *"), std::string::npos);
    EXPECT_NE(s.find("Access-Control-Allow-Methods:"), std::string::npos);
    EXPECT_NE(s.find("Access-Control-Allow-Headers: Content-Type"), std::string::npos);
    EXPECT_NE(s.find("Connection: close"), std::string::npos);

    // Body at the end
    auto hdr_end = s.find("\r\n\r\n");
    ASSERT_NE(hdr_end, std::string::npos);
    EXPECT_EQ(s.substr(hdr_end + 4), resp.body);

    // 404
    net::HttpResponse nf;
    nf.status_code = 404;
    nf.body = "Not Found";
    EXPECT_EQ(nf.serialize().find("HTTP/1.1 404 Not Found"), 0u);

    // 204 empty
    net::HttpResponse nc;
    nc.status_code = 204;
    EXPECT_EQ(nc.serialize().find("HTTP/1.1 204 No Content"), 0u);
    EXPECT_NE(nc.serialize().find("Content-Length: 0"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────
// Test 5: CorsPreflightResponse
// ─────────────────────────────────────────────────────────────
TEST(HttpResponseTest, CorsPreflightResponse) {
    // Parse OPTIONS request
    std::string raw =
        "OPTIONS /api/strategies HTTP/1.1\r\n"
        "Host: localhost:9090\r\n"
        "Origin: http://example.com\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "\r\n";

    net::HttpRequest req;
    ASSERT_TRUE(net::HttpServer::parse_request(raw, req));
    EXPECT_EQ(req.method, "OPTIONS");
    EXPECT_EQ(req.path, "/api/strategies");

    // Simulate server's CORS preflight response
    net::HttpResponse resp;
    resp.status_code = 204;
    resp.body = "";
    std::string s = resp.serialize();

    EXPECT_EQ(s.find("HTTP/1.1 204 No Content"), 0u);
    EXPECT_NE(s.find("Access-Control-Allow-Origin: *"), std::string::npos);
    EXPECT_NE(s.find("Access-Control-Allow-Methods:"), std::string::npos);
    EXPECT_NE(s.find("Access-Control-Allow-Headers: Content-Type"), std::string::npos);
    EXPECT_NE(s.find("Content-Length: 0"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────
// Test 6: HttpServerStartStop
// ─────────────────────────────────────────────────────────────
TEST(HttpServerTest, StartStop) {
    net::HttpServerConfig cfg;
    cfg.port = 19090;
    cfg.host = "127.0.0.1";

    net::HttpServer server(cfg);
    EXPECT_FALSE(server.is_running());

    server.set_handler([](const net::HttpRequest&) -> net::HttpResponse {
        return {200, "ok", "text/plain"};
    });

    // Start
    EXPECT_TRUE(server.start());
    EXPECT_TRUE(server.is_running());

    // Double start is idempotent
    EXPECT_TRUE(server.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Stop
    server.stop();
    EXPECT_FALSE(server.is_running());

    // Double stop is idempotent
    server.stop();
    EXPECT_FALSE(server.is_running());
}

// ─────────────────────────────────────────────────────────────
// Test 7: HttpServerRouting — real TCP round-trip
// ─────────────────────────────────────────────────────────────
TEST(HttpServerTest, Routing) {
    net::HttpServerConfig cfg;
    cfg.port = 19091;
    cfg.host = "127.0.0.1";

    std::string captured_method;
    std::string captured_path;
    std::string captured_body;

    net::HttpServer server(cfg);
    server.set_handler([&](const net::HttpRequest& req) -> net::HttpResponse {
        captured_method = req.method;
        captured_path = req.path;
        captured_body = req.body;
        return net::HttpResponse{201, R"({"id":"abc"})", "application/json"};
    });

    ASSERT_TRUE(server.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send POST with JSON body
    std::string body = R"({"name":"my_strategy"})";
    std::string request =
        "POST /api/strategies HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    std::string response = send_http(19091, request);
    EXPECT_FALSE(response.empty());

    // Handler was called with correct data
    EXPECT_EQ(captured_method, "POST");
    EXPECT_EQ(captured_path, "/api/strategies");
    EXPECT_EQ(captured_body, body);

    // Response has correct status and body
    EXPECT_NE(response.find("HTTP/1.1 201 Created"), std::string::npos);
    EXPECT_NE(response.find(R"({"id":"abc"})"), std::string::npos);

    // Stats
    auto stats = server.stats();
    EXPECT_GE(stats.requests_received, 1u);
    EXPECT_GE(stats.requests_served, 1u);

    server.stop();
}
