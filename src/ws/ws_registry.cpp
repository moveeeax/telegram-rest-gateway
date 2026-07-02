#include "ws/ws_registry.hpp"

#include <algorithm>

namespace tgw::ws {

WsSubscriberRegistry& WsSubscriberRegistry::instance() {
    static WsSubscriberRegistry registry;
    return registry;
}

void WsSubscriberRegistry::connect(const drogon::WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.push_back(conn);
}

void WsSubscriberRegistry::disconnect(const drogon::WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(std::remove(connections_.begin(), connections_.end(), conn),
                       connections_.end());
}

void WsSubscriberRegistry::fanOut(const std::string& payload) {
    std::vector<drogon::WebSocketConnectionPtr> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = connections_;
    }
    for (const auto& conn : snapshot) {
        if (conn->connected()) {
            conn->send(payload);
        }
    }
}

std::size_t WsSubscriberRegistry::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

}  // namespace tgw::ws
