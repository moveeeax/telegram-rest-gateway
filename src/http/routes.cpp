#include "http/routes.hpp"

#include "bridge/expect.hpp"
#include "bridge/td_bridge.hpp"

#include <drogon/drogon.h>
#include <td/telegram/td_api.h>

#include <functional>
#include <string>
#include <utility>

namespace td_api = td::td_api;

namespace tgw::http {
namespace {

// Тонкая проекция td_api::user (§8.2.2). id — СТРОКОЙ (§8.2.1).
Json::Value userToJson(const td_api::user& user) {
    Json::Value json;
    json["id"] = std::to_string(user.id_);
    json["first_name"] = user.first_name_;
    json["last_name"] = user.last_name_;
    return json;
}

drogon::HttpResponsePtr jsonResponse(Json::Value body, drogon::HttpStatusCode code) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    resp->setStatusCode(code);
    return resp;
}

drogon::HttpResponsePtr telegramError(const td_api::error& error) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = "TELEGRAM_ERROR";
    body["error"]["message"] = error.message_;
    body["error"]["tdlib_code"] = error.code_;
    body["error"]["tdlib_message"] = error.message_;
    // Минимальный маппинг этапа 1; полная таблица кодов -> HTTP — §9.1 (этап 3).
    return jsonResponse(std::move(body), drogon::k502BadGateway);
}

void registerHealth() {
    drogon::app().registerHandler(
        "/v1/health",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            Json::Value body;
            body["ok"] = true;
            body["status"] = "alive";
            callback(drogon::HttpResponse::newHttpJsonResponse(body));
        },
        {drogon::Get});
}

}  // namespace

void registerRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id) {
    registerHealth();

    // GET /v1/me — PoC моста: co_await ответа TDLib через correlation map (§12, этап 1).
    // До авторизации (этап 2) TDLib вернёт error — это тоже доказывает работу моста
    // (запрос -> ответ маршрутизируется обратно в приостановленный HTTP-запрос).
    // registerHandler не биндит корутинные лямбды (FunctionTraits их не распознаёт),
    // поэтому внешний хендлер — обычный callback, а корутину гоняем отдельным
    // detached AsyncTask (захват по значению callback/client_id, bridge — по ссылке).
    drogon::app().registerHandler(
        "/v1/me",
        [&bridge, client_id](const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            [](tgw::bridge::TdBridge& td, std::int32_t cid,
               std::function<void(const drogon::HttpResponsePtr&)> cb) -> drogon::AsyncTask {
                auto object = co_await td.invoke(cid, td_api::make_object<td_api::getMe>());
                auto result = tgw::bridge::expect<td_api::user>(std::move(object));
                if (!result.ok()) {
                    cb(telegramError(*result.error));
                    co_return;
                }
                Json::Value body;
                body["ok"] = true;
                body["data"] = userToJson(*result.value);
                cb(jsonResponse(std::move(body), drogon::k200OK));
                co_return;
            }(bridge, client_id, std::move(callback));
        },
        {drogon::Get});
}

}  // namespace tgw::http
