#include "ws/updates_ws.hpp"

#include "auth/token_store.hpp"
#include "ws/ws_registry.hpp"

#include <json/value.h>
#include <json/writer.h>
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

std::string helloFrame() {
    Json::Value frame;
    frame["type"] = "hello";
    frame["session_id"] = "default";
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, frame);
}

}  // namespace

void UpdatesWs::handleNewConnection(const drogon::HttpRequestPtr& req,
                                    const drogon::WebSocketConnectionPtr& conn) {
    if (!tgw::auth::TokenStore::instance().verify(extractBearer(req))) {
        conn->forceClose();
        return;
    }
    WsSubscriberRegistry::instance().connect(conn);
    conn->send(helloFrame());
}

void UpdatesWs::handleNewMessage(const drogon::WebSocketConnectionPtr& /*conn*/,
                                 std::string&& /*message*/,
                                 const drogon::WebSocketMessageType& /*type*/) {
    // MVP: входящие фреймы (ping/pong/подписки) игнорируем, не падаем (§6.5).
}

void UpdatesWs::handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) {
    WsSubscriberRegistry::instance().disconnect(conn);
}

}  // namespace tgw::ws
