#include "ws/ws_registry.hpp"

#include "util/metrics.hpp"

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <vector>

namespace tgw::ws {

WsSubscriberRegistry& WsSubscriberRegistry::instance() {
    static WsSubscriberRegistry registry;
    return registry;
}

void WsSubscriberRegistry::connect(const drogon::WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.push_back(Subscriber{conn, 0});
}

void WsSubscriberRegistry::disconnect(const drogon::WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                      [&conn](const Subscriber& s) { return s.conn == conn; }),
                       connections_.end());
}

void WsSubscriberRegistry::notePong(const drogon::WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : connections_) {
        if (s.conn == conn) {
            s.bytes_since_pong = 0;
            return;
        }
    }
}

void WsSubscriberRegistry::setFilter(const drogon::WebSocketConnectionPtr& conn,
                                     std::set<std::string> types) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : connections_) {
        if (s.conn == conn) {
            s.filter = std::move(types);
            return;
        }
    }
}

void WsSubscriberRegistry::setMaxPendingBytes(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_pending_bytes_ = bytes;
}

void WsSubscriberRegistry::fanOut(const std::string& update_type, const std::string& payload) {
    std::vector<drogon::WebSocketConnectionPtr> alive;
    std::vector<drogon::WebSocketConnectionPtr> slow;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        alive.reserve(connections_.size());
        for (auto& s : connections_) {
            if (!s.conn->connected()) {
                continue;
            }
            if (!s.filter.empty() && s.filter.count(update_type) == 0) {
                continue;  // подписка отфильтровала этот тип (в backlog не считаем — не шлём)
            }
            s.bytes_since_pong += payload.size();
            if (max_pending_bytes_ != 0 && s.bytes_since_pong > max_pending_bytes_) {
                slow.push_back(s.conn);  // отключим вне лока
            } else {
                alive.push_back(s.conn);
            }
        }
        if (!slow.empty()) {
            connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                              [&slow](const Subscriber& s) {
                                                  return std::find(slow.begin(), slow.end(),
                                                                   s.conn) != slow.end();
                                              }),
                               connections_.end());
        }
    }
    // send/forceClose — вне лока: не задерживаем connect/disconnect других соединений.
    for (const auto& conn : slow) {
        LOG_WARN << "ws back-pressure: slow client " << conn->peerAddr().toIpPort()
                 << " exceeded pending-bytes limit, disconnecting";
        tgw::metrics::Counters::instance().ws_slow_disconnects_total.fetch_add(
            1, std::memory_order_relaxed);
        conn->forceClose();
    }
    for (const auto& conn : alive) {
        conn->send(payload);
    }
}

std::size_t WsSubscriberRegistry::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

}  // namespace tgw::ws
