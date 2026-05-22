// ws_session.cc — Session management implementation
#include "cpp/quant/network/ws_session.h"

#include <algorithm>

#include "cpp/quant/infra/coroutine.h"
#include <chrono>
#include <sstream>

namespace quant::network {

std::string SessionManager::generate_session_id() {
    auto id = session_seq_.fetch_add(1, std::memory_order_relaxed);
    return "session_" + std::to_string(id);
}

std::string SessionManager::create_session(const std::string& remote_addr, int remote_port) {
    auto lock = infra::blockingWait(mutex_.co_scoped_lock());
    std::string id = generate_session_id();
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    SessionInfo info;
    info.session_id = id;
    info.remote_addr = remote_addr;
    info.remote_port = remote_port;
    info.state = SessionState::kHandshaking;
    info.connected_at_ns = now_ns;
    info.last_activity_ns = now_ns;
    sessions_.emplace(id, std::move(info));
    return id;
}

bool SessionManager::close_session(const std::string& session_id) {
    auto lock = infra::blockingWait(mutex_.co_scoped_lock());
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    for (const auto& topic : it->second.subscriptions) {
        auto tit = topic_subscribers_.find(topic);
        if (tit != topic_subscribers_.end()) {
            auto& ids = tit->second;
            ids.erase(std::remove(ids.begin(), ids.end(), session_id), ids.end());
        }
    }
    sessions_.erase(it);
    if (callbacks_.on_close) callbacks_.on_close(session_id);
    return true;
}

SessionInfo* SessionManager::find_session(const std::string& session_id) {
    auto lock = infra::blockingWait(mutex_.co_scoped_lock());
    auto it = sessions_.find(session_id);
    return it != sessions_.end() ? &it->second : nullptr;
}

bool SessionManager::authenticate(const std::string& session_id, const std::string& token) {
    auto lock = infra::blockingWait(mutex_.co_scoped_lock());
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    if (!callbacks_.on_auth) {
        it->second.state = SessionState::kActive;
        return true;
    }
    std::string user_id;
    if (callbacks_.on_auth(token, user_id)) {
        it->second.state = SessionState::kActive;
        it->second.auth_token = token;
        it->second.user_id = user_id;
        return true;
    }
    return false;
}

bool SessionManager::subscribe(const std::string& session_id, const std::string& topic) {
    auto lock = infra::blockingWait(mutex_.co_scoped_lock());
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    it->second.subscriptions.insert(topic);
    topic_subscribers_[topic].push_back(session_id);
    if (callbacks_.on_subscribe) callbacks_.on_subscribe(session_id, topic);
    return true;
}

bool SessionManager::unsubscribe(const std::string& session_id, const std::string& topic) {
    auto lock = infra::blockingWait(mutex_.co_scoped_lock());
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    it->second.subscriptions.erase(topic);
    auto tit = topic_subscribers_.find(topic);
    if (tit != topic_subscribers_.end()) {
        auto& ids = tit->second;
        ids.erase(std::remove(ids.begin(), ids.end(), session_id), ids.end());
    }
    if (callbacks_.on_unsubscribe) callbacks_.on_unsubscribe(session_id, topic);
    return true;
}

std::vector<std::string> SessionManager::session_ids_for_topic(const std::string& topic) const {
    auto lock = infra::blockingWait(mutex_.co_scoped_lock());
    auto it = topic_subscribers_.find(topic);
    return it != topic_subscribers_.end() ? it->second : std::vector<std::string>();
}

std::vector<std::string> SessionManager::active_session_ids() const {
    auto lock = infra::blockingWait(mutex_.co_scoped_lock());
    std::vector<std::string> result;
    result.reserve(sessions_.size());
    for (const auto& [id, info] : sessions_) {
        if (info.state == SessionState::kActive) result.push_back(id);
    }
    return result;
}

void SessionManager::cleanup_expired(int64_t now_ns, int64_t timeout_ns) {
    auto lock = infra::blockingWait(mutex_.co_scoped_lock());
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (now_ns - it->second.last_activity_ns > timeout_ns) {
            if (callbacks_.on_close) callbacks_.on_close(it->first);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace quant::network
