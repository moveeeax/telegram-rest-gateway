#pragma once

#include <drogon/WebSocketController.h>

#include <string>

namespace tgw::ws {

// WS /v1/updates — поток апдейтов (§6.3). Корутины в WS-контроллерах Drogon не поддержаны,
// поэтому только колбэки. Аутентификация Bearer при рукопожатии; подписка на все апдейты сессии.
class UpdatesWs : public drogon::WebSocketController<UpdatesWs> {
   public:
    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override;
    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                          const drogon::WebSocketMessageType& type) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/v1/updates");
    WS_PATH_LIST_END
};

}  // namespace tgw::ws
