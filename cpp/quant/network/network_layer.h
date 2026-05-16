// network_layer.h — Network layer facade combining TCP + WebSocket
#pragma once

#include <memory>
#include <string>

#include "cpp/quant/network/tcp_connection.h"
#include "cpp/quant/network/websocket_server.h"
#include "cpp/quant/network/ws_session.h"

namespace quant::network {

struct NetworkConfig {
    WsServerConfig ws_config;
    bool enable_tcp = true;
    bool enable_ws  = true;
    int  event_loop_threads = 2;
};

class NetworkLayer {
public:
    explicit NetworkLayer(NetworkConfig config)
        : config_(std::move(config))
        , ws_server_(config_.ws_config)
        , session_manager_(std::make_shared<SessionManager>()) {}

    ~NetworkLayer() { stop(); }

    NetworkLayer(const NetworkLayer&) = delete;
    NetworkLayer& operator=(const NetworkLayer&) = delete;

    bool start() {
        if (config_.enable_ws) {
            if (!ws_server_.start()) return false;
        }
        running_ = true;
        return true;
    }

    void stop() noexcept {
        if (config_.enable_ws) ws_server_.stop();
        running_ = false;
    }

    bool is_running() const noexcept { return running_; }

    WebSocketServer& ws_server() noexcept { return ws_server_; }
    std::shared_ptr<SessionManager> session_manager() noexcept { return session_manager_; }

private:
    NetworkConfig config_;
    WebSocketServer ws_server_;
    std::shared_ptr<SessionManager> session_manager_;
    bool running_ = false;
};

}  // namespace quant::network
