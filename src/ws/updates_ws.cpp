#include "ws/updates_ws.hpp"

#include "auth/token_store.hpp"
#include "ws/ws_registry.hpp"

#include <chrono>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace tgw::ws {
namespace {

// Bearer из рукопожатия: заголовок Authorization: Bearer <t> или Sec-WebSocket-Protocol: bearer.<t>
// (для браузерного WebSocket, не умеющего произвольные заголовки, §8.1). Долгоживущий токен в
// query запрещён (утечка в логи).
std::string extractBearer(const drogon::HttpRequestPtr& req) {
    const std::string& authz = req->getHeader("Authorization");
    constexpr std::string_view kBearer = "Bearer ";
    if (authz.size() > kBearer.size() &&
        std::string_view(authz).substr(0, kBearer.size()) == kBearer) {
        return authz.substr(kBearer.size());
    }
    const std::string& proto = req->getHeader("Sec-WebSocket-Protocol");
    constexpr std::string_view kProtoBearer = "bearer.";
    if (proto.size() > kProtoBearer.size() &&
        std::string_view(proto).substr(0, kProtoBearer.size()) == kProtoBearer) {
        return proto.substr(kProtoBearer.size());
    }
    return "";
}

std::string& sessionIdRef() {
    static std::string session_id = "default";
    return session_id;
}

std::string helloFrame() {
    Json::Value frame;
    frame["type"] = "hello";
    frame["session_id"] = sessionIdRef();
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, frame);
}

}  // namespace

void UpdatesWs::setSessionId(std::string session_id) {
    sessionIdRef() = std::move(session_id);
}

void UpdatesWs::handleNewConnection(const drogon::HttpRequestPtr& req,
                                    const drogon::WebSocketConnectionPtr& conn) {
    const auto scopes = tgw::auth::TokenStore::instance().verify(extractBearer(req));
    if (!scopes.has_value() || !tgw::auth::scopeAllows(*scopes, tgw::auth::Scope::Read)) {
        conn->forceClose();
        return;
    }
    WsSubscriberRegistry::instance().connect(conn);
    // Пинг раз в 15с — драйвер back-pressure: pong сбрасывает счётчик backlog'а (ws_registry).
    conn->setPingMessage("", std::chrono::seconds(15));
    conn->send(helloFrame());
}

void UpdatesWs::handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                                 const drogon::WebSocketMessageType& type) {
    // Pong = клиент дочитал поток до нашего пинга — снимаем его backlog-счётчик.
    if (type == drogon::WebSocketMessageType::Pong) {
        WsSubscriberRegistry::instance().notePong(conn);
        return;
    }
    // Подписка с фильтром: {"type":"subscribe","update_types":["updateNewMessage",...]}.
    // Пустой массив = все типы. Прочие/битые фреймы игнорируем, не падаем (§6.5).
    if (type != drogon::WebSocketMessageType::Text) {
        return;
    }
    Json::Value frame;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(message.data(), message.data() + message.size(), &frame, &errs)) {
        return;
    }
    // Топ-левел ДОЛЖЕН быть объектом: у jsoncpp Value::operator[](const char*) на массиве/скаляре
    // кидает Json::LogicError, а колбэк WS-сообщения Drogon ничем не обёрнут — исключение ушло бы
    // в event-loop и убило процесс. Тем же соображением asString() зовём только для строки.
    if (!frame.isObject() || !frame["type"].isString()) {
        return;
    }
    if (frame["type"].asString() != "subscribe" || !frame["update_types"].isArray()) {
        return;
    }
    std::set<std::string> types;
    for (const auto& t : frame["update_types"]) {
        if (t.isString()) {
            types.insert(t.asString());
        }
    }
    WsSubscriberRegistry::instance().setFilter(conn, types);
    Json::Value ack;
    ack["type"] = "subscribed";
    ack["update_types"] = frame["update_types"];
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    conn->send(Json::writeString(w, ack));
}

void UpdatesWs::handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) {
    WsSubscriberRegistry::instance().disconnect(conn);
}

}  // namespace tgw::ws
