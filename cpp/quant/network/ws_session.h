// ws_session.h — WebSocket session management
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quant::network {

enum class SessionState : uint8_t {
    kHandshaking   = 0,
    kAuthenticating = 1,
    kActive        = 2,
    kClosing       = 3,
    kClosed        = 4,
};

struct SessionInfo {
    std::string     session_id;
    std::string     remote_addr;
    int             remote_port = 0;
    SessionState    state       = SessionState::kHandshaking;
    int64_t         connected_at_ns = 0;
    int64_t         last_activity_ns = 0;
    std::string     auth_token;
    std::string     user_id;
    std::unordered_set<std::string> subscriptions;
};

struct SessionCallbacks {
    std::function<bool(const std::string& token, std::string& user_id)> on_auth;
    std::function<void(const std::string& session_id, const std::string& topic)> on_subscribe;
    std::function<void(const std::string& session_id, const std::string& topic)> on_unsubscribe;
    std::function<void(const std::string& session_id)> on_close;
};

class SessionManager {
public:
    SessionManager() = default;

    std::string create_session(const std::string& remote_addr, int remote_port);
    bool close_session(const std::string& session_id);
    SessionInfo* find_session(const std::string& session_id);

    bool authenticate(const std::string& session_id, const std::string& token);

    bool subscribe(const std::string& session_id, const std::string& topic);
    bool unsubscribe(const std::string& session_id, const std::string& topic);
    std::vector<std::string> session_ids_for_topic(const std::string& topic) const;

    size_t active_session_count() const noexcept { return sessions_.size(); }
    std::vector<std::string> active_session_ids() const;

    void set_callbacks(SessionCallbacks callbacks) { callbacks_ = std::move(callbacks); }
    void cleanup_expired(int64_t now_ns, int64_t timeout_ns);

private:
    std::string generate_session_id();
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionInfo> sessions_;
    std::unordered_map<std::string, std::vector<std::string>> topic_subscribers_;
    SessionCallbacks callbacks_;
    std::atomic<uint64_t> session_seq_{0};
};

}  // namespace quant::network
