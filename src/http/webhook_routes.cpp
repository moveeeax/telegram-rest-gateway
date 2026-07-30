#include "http/webhook_routes.hpp"

#include "http/http_helpers.hpp"
#include "http/route_table.hpp"
#include "webhook/webhook_registry.hpp"

#include <drogon/drogon.h>

#include <string>
#include <utility>

// jsonResponse/serviceError — общие, из http/http_helpers.hpp (решение 1.6). Пути и constraints
// (метод + фильтр kBearerFilter, admin-скоуп — из scope_policy.hpp) берутся из
// http/route_table.hpp — единого источника истины (решение ревью 3.7): здесь фильтр руками не
// навешивается.
//
// ВЕЗДЕ, где читаем тело: json->isObject() ПЕРЕД operator[](const char*)/isMember — тело вида
// `[]`/`"x"`/`5` тоже валидный JSON-документ, а Value::operator[](const char*) и Value::isMember
// на не-объекте кидают Json::LogicError (см. message_routes.cpp, тот же приём).

namespace tgw::http {
namespace {

// Проекция Webhook в JSON БЕЗ secret (§ ответ GET /v1/webhooks — секрет наружу не отдаём).
Json::Value webhookToJson(const tgw::webhook::Webhook& hook) {
    Json::Value json;
    json["id"] = hook.id;
    json["url"] = hook.url;
    json["active"] = hook.active;
    return json;
}

}  // namespace

drogon::HttpResponsePtr handleWebhookCreate(const drogon::HttpRequestPtr& req,
                                            tgw::webhook::WebhookRegistry& registry) {
    auto json = req->getJsonObject();
    if (json == nullptr || !json->isObject() || !(*json)["url"].isString() ||
        (*json)["url"].asString().empty()) {
        return serviceError("VALIDATION_ERROR", "field 'url' is required", drogon::k400BadRequest);
    }
    const std::string url = (*json)["url"].asString();
    const std::string secret = (*json)["secret"].isString() ? (*json)["secret"].asString() : "";
    // active по умолчанию true; отсутствующее/нелогическое значение трактуем как «не задано»
    // (тот же приём, что revoke_ в message_routes.cpp DELETE /v1/chats/{chatId}/messages).
    const bool active = !(*json)["active"].isBool() || (*json)["active"].asBool();

    const std::string id = registry.add(url, secret, active);

    Json::Value data;
    data["id"] = id;
    data["url"] = url;
    data["active"] = active;
    Json::Value body;
    body["ok"] = true;
    body["data"] = data;
    return jsonResponse(std::move(body), drogon::k200OK);
}

void registerWebhookRoutes(tgw::webhook::WebhookRegistry& registry) {
    auto& app = drogon::app();

    // Регистрация строго через таблицу маршрутов (http/route_table.hpp), как в routes.cpp
    // (решение ревью 3.7): путь и constraints (метод + kBearerFilter для защищённых) берутся из
    // RouteSpec — фильтр здесь руками не навешивается.
    auto registerRoute = [&app](const RouteSpec& spec, auto&& handler) {
        app.registerHandler(std::string(spec.path), std::forward<decltype(handler)>(handler),
                            constraintsFor(spec));
    };

    // POST /v1/webhooks — регистрация нового вебхука. Тело: {"url", "secret"?, "active"?}.
    // Разбор/валидация вынесены в handleWebhookCreate (тестируется без Drogon-роутера).
    registerRoute(kWebhookCreateRoute,
                 [&registry](const drogon::HttpRequestPtr& req, HttpCallback&& cb) {
                     cb(handleWebhookCreate(req, registry));
                 });

    // GET /v1/webhooks — список БЕЗ secret (webhookToJson secret не проецирует).
    registerRoute(kWebhookListRoute,
                 [&registry](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
                     Json::Value arr(Json::arrayValue);
                     for (const auto& hook : registry.list()) {
                         arr.append(webhookToJson(hook));
                     }
                     Json::Value body;
                     body["ok"] = true;
                     body["data"] = arr;
                     cb(jsonResponse(std::move(body), drogon::k200OK));
                 });

    // DELETE /v1/webhooks/{id} — id вебхука строковый (hex(sha256(url))[0:16], см.
    // webhook_registry.hpp), НЕ десятичное число: http_helpers::parseId здесь неприменим,
    // path-параметр используется как есть. 200 — удалили; 404 — такого id не было.
    registerRoute(kWebhookDeleteRoute,
                 [&registry](const drogon::HttpRequestPtr&, HttpCallback&& cb, std::string id) {
                     if (!registry.remove(id)) {
                         cb(serviceError("NOT_FOUND", "webhook not found", drogon::k404NotFound));
                         return;
                     }
                     Json::Value body;
                     body["ok"] = true;
                     cb(jsonResponse(std::move(body), drogon::k200OK));
                 });
}

}  // namespace tgw::http
