#pragma once

#include <drogon/WebSocketConnection.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace tgw::ws {

// Реестр активных WS-подписчиков (§6.3′). Fan-out выполняется прямо из потока-приёмника:
// снимок под коротким локом, затем conn->send (потокобезопасен, порядок сохраняется, т.к.
// шлёт единственный поток-приёмник). Отключённые соединения пропускаются.
//
// Back-pressure: публичный API Drogon не даёт заглянуть в исходящий буфер, поэтому backlog
// оцениваем по pong'ам. Сервер пингует клиента (updates_ws, kPingIntervalSeconds); пинг встаёт
// в TCP-поток ЗА уже отправленными данными, так что клиент, не вычитывающий апдейты, не увидит
// пинг и не ответит. «Байты с последнего pong» ≈ невычитанный backlog: превысил лимит —
// принудительно отключаем (защита памяти от медленного/зависшего клиента).
class WsSubscriberRegistry {
   public:
    static WsSubscriberRegistry& instance();

    void connect(const drogon::WebSocketConnectionPtr& conn);
    void disconnect(const drogon::WebSocketConnectionPtr& conn);
    // Клиент ответил pong'ом — жив и вычитывает; сбрасываем его счётчик backlog'а.
    void notePong(const drogon::WebSocketConnectionPtr& conn);
    // Фильтр подписки соединения: пустой set = все типы (default).
    void setFilter(const drogon::WebSocketConnectionPtr& conn, std::set<std::string> types);
    void fanOut(const std::string& update_type, const std::string& payload);
    std::size_t size();

    // Лимит «байт с последнего pong» до отключения. 0 — back-pressure выключен.
    void setMaxPendingBytes(std::uint64_t bytes);

   private:
    struct Subscriber {
        drogon::WebSocketConnectionPtr conn;
        std::uint64_t bytes_since_pong = 0;
        std::set<std::string> filter;  // пусто = все типы апдейтов
    };

    std::mutex mutex_;
    std::vector<Subscriber> connections_;
    std::uint64_t max_pending_bytes_ = 8ULL * 1024 * 1024;  // 8 MiB по умолчанию
};

}  // namespace tgw::ws
