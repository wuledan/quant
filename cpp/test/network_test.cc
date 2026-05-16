// network_test.cc — Tests for network layer components
#include "cpp/quant/network/tcp_connection.h"
#include "cpp/quant/network/websocket_server.h"
#include "cpp/quant/network/ws_session.h"
#include "cpp/quant/network/network_layer.h"
#include <gtest/gtest.h>
#include <cstring>

namespace quant::network {
namespace {

TEST(IoBuffer, BasicReadWrite) {
    IoBuffer buf(1024);
    EXPECT_EQ(buf.capacity(), 1024u);
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable(), 1024u);

    const char* data = "hello";
    std::memcpy(buf.write_data(), data, 5);
    buf.produced(5);
    EXPECT_EQ(buf.readable(), 5u);

    buf.consume(3);
    EXPECT_EQ(buf.readable(), 2u);
}

TEST(IoBuffer, Clear) {
    IoBuffer buf;
    const char* data = "test";
    std::memcpy(buf.write_data(), data, 4);
    buf.produced(4);
    buf.clear();
    EXPECT_EQ(buf.readable(), 0u);
}

TEST(IoBuffer, Grow) {
    IoBuffer buf(64);
    buf.grow(128);
    EXPECT_GE(buf.capacity(), 128u);
}

TEST(WsFrameUtils, EncodeDecodeTextFrame) {
    std::string msg = "Hello, WebSocket!";
    auto frame = encode_ws_frame(
        reinterpret_cast<const uint8_t*>(msg.data()),
        msg.size(), WsOpcode::kText);
    EXPECT_GT(frame.size(), msg.size());

    WsFrame parsed;
    bool ok = parse_ws_frame(frame.data(), frame.size(), parsed);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(parsed.fin);
    EXPECT_EQ(parsed.opcode, WsOpcode::kText);
    EXPECT_FALSE(parsed.masked);
    EXPECT_EQ(parsed.payload_len, msg.size());
    EXPECT_EQ(parsed.payload.size(), msg.size());
    EXPECT_EQ(std::string(parsed.payload.begin(), parsed.payload.end()), msg);
}

TEST(WsFrameUtils, EncodeDecodeBinary) {
    std::vector<uint8_t> data = {0x00, 0x01, 0x02, 0xFF, 0xFE};
    auto frame = encode_ws_frame(data.data(), data.size(), WsOpcode::kBinary);

    WsFrame parsed;
    bool ok = parse_ws_frame(frame.data(), frame.size(), parsed);
    EXPECT_TRUE(ok);
    EXPECT_EQ(parsed.opcode, WsOpcode::kBinary);
    EXPECT_EQ(parsed.payload.size(), data.size());
    EXPECT_TRUE(std::equal(parsed.payload.begin(), parsed.payload.end(), data.begin()));
}

TEST(WsFrameUtils, IncompleteFrame) {
    std::string msg = "test";
    auto frame = encode_ws_frame(
        reinterpret_cast<const uint8_t*>(msg.data()),
        msg.size(), WsOpcode::kText);

    WsFrame parsed;
    bool ok = parse_ws_frame(frame.data(), 1, parsed);
    EXPECT_FALSE(ok);
}

TEST(WsFrameUtils, EmptyFrame) {
    WsFrame parsed;
    bool ok = parse_ws_frame(nullptr, 0, parsed);
    EXPECT_FALSE(ok);
}

TEST(SessionManager, CreateSession) {
    SessionManager mgr;
    auto id = mgr.create_session("127.0.0.1", 8080);
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(mgr.active_session_count(), 1u);
}

TEST(SessionManager, CloseSession) {
    SessionManager mgr;
    auto id = mgr.create_session("127.0.0.1", 8080);
    EXPECT_TRUE(mgr.close_session(id));
    EXPECT_EQ(mgr.active_session_count(), 0u);
}

TEST(SessionManager, FindSession) {
    SessionManager mgr;
    auto id = mgr.create_session("127.0.0.1", 8080);
    auto* info = mgr.find_session(id);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->remote_addr, "127.0.0.1");
    EXPECT_EQ(info->remote_port, 8080);
    EXPECT_EQ(info->state, SessionState::kHandshaking);
}

TEST(SessionManager, Authenticate) {
    SessionManager mgr;
    auto id = mgr.create_session("127.0.0.1", 8080);
    EXPECT_TRUE(mgr.authenticate(id, "token"));
    auto* info = mgr.find_session(id);
    EXPECT_EQ(info->state, SessionState::kActive);
}

TEST(SessionManager, SubscribeAndUnsubscribe) {
    SessionManager mgr;
    auto id = mgr.create_session("127.0.0.1", 8080);
    mgr.authenticate(id, "token");

    EXPECT_TRUE(mgr.subscribe(id, "market.600000.SS"));
    EXPECT_TRUE(mgr.subscribe(id, "trade.report"));

    auto subs = mgr.session_ids_for_topic("market.600000.SS");
    EXPECT_EQ(subs.size(), 1u);
    EXPECT_EQ(subs[0], id);

    EXPECT_TRUE(mgr.unsubscribe(id, "market.600000.SS"));
    subs = mgr.session_ids_for_topic("market.600000.SS");
    EXPECT_TRUE(subs.empty());
}

TEST(SessionManager, MultipleSubscribers) {
    SessionManager mgr;
    auto id1 = mgr.create_session("127.0.0.1", 8081);
    auto id2 = mgr.create_session("127.0.0.1", 8082);
    mgr.authenticate(id1, "t1");
    mgr.authenticate(id2, "t2");

    mgr.subscribe(id1, "market.600000.SS");
    mgr.subscribe(id2, "market.600000.SS");

    auto subs = mgr.session_ids_for_topic("market.600000.SS");
    EXPECT_EQ(subs.size(), 2u);
}

TEST(SessionManager, ActiveSessionIds) {
    SessionManager mgr;
    auto id = mgr.create_session("127.0.0.1", 8080);
    EXPECT_TRUE(mgr.active_session_ids().empty());

    mgr.authenticate(id, "t");
    auto active = mgr.active_session_ids();
    EXPECT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0], id);
}

TEST(NetworkLayer, CreateAndStop) {
    NetworkConfig cfg;
    cfg.enable_ws = false;
    NetworkLayer layer(cfg);
    EXPECT_FALSE(layer.is_running());
    EXPECT_TRUE(layer.start());
    EXPECT_TRUE(layer.is_running());
    layer.stop();
    EXPECT_FALSE(layer.is_running());
}

TEST(WebSocketServer, CreateAndConfig) {
    WsServerConfig cfg;
    cfg.port = 0;
    WebSocketServer server(cfg);
    EXPECT_FALSE(server.is_running());
}

}  // namespace
}  // namespace quant::network