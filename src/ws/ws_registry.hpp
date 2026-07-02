#pragma once

#include <drogon/WebSocketConnection.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace tgw::ws {

// Реестр активных WS-подписчиков (§6.3′). Fan-out выполняется прямо из потока-приёмника:
// снимок под коротким локом, затем conn->send (потокобезопасен, порядок сохраняется, т.к.
// шлёт единственный поток-приёмник). Отключённые соединения пропускаются.
class WsSubscriberRegistry {
   public:
    static WsSubscriberRegistry& instance();

    void connect(const drogon::WebSocketConnectionPtr& conn);
    void disconnect(const drogon::WebSocketConnectionPtr& conn);
    void fanOut(const std::string& payload);
    std::size_t size();

   private:
    std::mutex mutex_;
    std::vector<drogon::WebSocketConnectionPtr> connections_;
};

}  // namespace tgw::ws
