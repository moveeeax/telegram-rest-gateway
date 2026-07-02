#include "http/routes.hpp"

#include "auth/auth_state_manager.hpp"
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

using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;

drogon::HttpResponsePtr jsonResponse(Json::Value body, drogon::HttpStatusCode code) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    resp->setStatusCode(code);
    return resp;
}

drogon::HttpResponsePtr serviceError(const std::string& code, const std::string& message,
                                     drogon::HttpStatusCode http) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = code;
    body["error"]["message"] = message;
    return jsonResponse(std::move(body), http);
}

drogon::HttpResponsePtr telegramError(const td_api::error& error, drogon::HttpStatusCode http) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = "TELEGRAM_ERROR";
    body["error"]["message"] = error.message_;
    body["error"]["tdlib_code"] = error.code_;
    body["error"]["tdlib_message"] = error.message_;
    return jsonResponse(std::move(body), http);
}

// Тонкая проекция td_api::user (§8.2.2). id — СТРОКОЙ (§8.2.1).
Json::Value userToJson(const td_api::user& user) {
    Json::Value json;
    json["id"] = std::to_string(user.id_);
    json["first_name"] = user.first_name_;
    json["last_name"] = user.last_name_;
    return json;
}

Json::Value authStateJson(const tgw::auth::AuthStateManager& auth) {
    const auto state = auth.current();
    Json::Value json;
    json["authorization_state"] = std::string(tgw::auth::toString(state));
    json["ready"] = (state == tgw::auth::AuthState::Ready);
    return json;
}

// Запускает detached-корутину для auth-мутации: co_await ответа, ok -> текущее состояние,
// error -> 400 с проброшенным td-кодом. registerHandler не биндит корутинные лямбды,
// поэтому корутину гоняем внутри обычного callback-хендлера (см. §5.4 и историю /v1/me).
void launchAuthMutation(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                        tgw::auth::AuthStateManager& auth, td_api::object_ptr<td_api::Function> fn,
                        HttpCallback callback) {
    [](tgw::bridge::TdBridge& td, std::int32_t cid, tgw::auth::AuthStateManager& a,
       td_api::object_ptr<td_api::Function> f, HttpCallback cb) -> drogon::AsyncTask {
        auto object = co_await td.invoke(cid, std::move(f));
        if (object != nullptr && object->get_id() == td_api::error::ID) {
            cb(telegramError(static_cast<td_api::error&>(*object), drogon::k400BadRequest));
            co_return;
        }
        Json::Value body;
        body["ok"] = true;
        body["data"] = authStateJson(a);
        cb(jsonResponse(std::move(body), drogon::k200OK));
        co_return;
    }(bridge, client_id, auth, std::move(fn), std::move(callback));
}

// Достаёт строковое поле из JSON-тела; при отсутствии/пустоте кладёт в err.
bool jsonString(const drogon::HttpRequestPtr& req, const char* field, std::string& out,
                std::string& err) {
    auto json = req->getJsonObject();
    if (json == nullptr || !json->isMember(field) || !(*json)[field].isString()) {
        err = std::string("field '") + field + "' is required";
        return false;
    }
    out = (*json)[field].asString();
    if (out.empty()) {
        err = std::string("field '") + field + "' must be non-empty";
        return false;
    }
    return true;
}

}  // namespace

void registerRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                    tgw::auth::AuthStateManager& auth) {
    auto& app = drogon::app();

    // --- system (без auth) ---
    app.registerHandler("/v1/health",
                        [](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
                            Json::Value body;
                            body["ok"] = true;
                            body["status"] = "alive";
                            cb(drogon::HttpResponse::newHttpJsonResponse(body));
                        },
                        {drogon::Get});

    app.registerHandler(
        "/v1/ready",
        [&auth](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
            const bool ready = (auth.current() == tgw::auth::AuthState::Ready);
            Json::Value body;
            body["ready"] = ready;
            body["state"] = std::string(tgw::auth::toString(auth.current()));
            cb(jsonResponse(std::move(body),
                            ready ? drogon::k200OK : drogon::k503ServiceUnavailable));
        },
        {drogon::Get});

    // --- auth (§7.2) — single-account, session_id неявно "default" ---
    app.registerHandler("/v1/auth/state",
                        [&auth](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
                            Json::Value body;
                            body["ok"] = true;
                            body["data"] = authStateJson(auth);
                            cb(jsonResponse(std::move(body), drogon::k200OK));
                        },
                        {drogon::Get});

    // setTdlibParameters уже отправлен StartupBootstrapper'ом; эндпоинт идемпотентен —
    // просто возвращает текущее состояние (§7.1).
    app.registerHandler("/v1/auth/session",
                        [&auth](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
                            Json::Value body;
                            body["ok"] = true;
                            body["data"] = authStateJson(auth);
                            cb(jsonResponse(std::move(body), drogon::k200OK));
                        },
                        {drogon::Post});

    app.registerHandler(
        "/v1/auth/phone",
        [&bridge, client_id, &auth](const drogon::HttpRequestPtr& req, HttpCallback&& cb) {
            std::string phone;
            std::string err;
            if (!jsonString(req, "phone_number", phone, err)) {
                cb(serviceError("VALIDATION_ERROR", err, drogon::k400BadRequest));
                return;
            }
            launchAuthMutation(
                bridge, client_id, auth,
                td_api::make_object<td_api::setAuthenticationPhoneNumber>(phone, nullptr),
                std::move(cb));
        },
        {drogon::Post});

    app.registerHandler(
        "/v1/auth/code",
        [&bridge, client_id, &auth](const drogon::HttpRequestPtr& req, HttpCallback&& cb) {
            std::string code;
            std::string err;
            if (!jsonString(req, "code", code, err)) {
                cb(serviceError("VALIDATION_ERROR", err, drogon::k400BadRequest));
                return;
            }
            launchAuthMutation(bridge, client_id, auth,
                               td_api::make_object<td_api::checkAuthenticationCode>(code),
                               std::move(cb));
        },
        {drogon::Post});

    app.registerHandler(
        "/v1/auth/password",
        [&bridge, client_id, &auth](const drogon::HttpRequestPtr& req, HttpCallback&& cb) {
            std::string password;
            std::string err;
            if (!jsonString(req, "password", password, err)) {
                cb(serviceError("VALIDATION_ERROR", err, drogon::k400BadRequest));
                return;
            }
            launchAuthMutation(bridge, client_id, auth,
                               td_api::make_object<td_api::checkAuthenticationPassword>(password),
                               std::move(cb));
        },
        {drogon::Post});

    // --- GET /v1/me (мост, §12 этап 1) ---
    app.registerHandler(
        "/v1/me",
        [&bridge, client_id](const drogon::HttpRequestPtr&, HttpCallback&& callback) {
            [](tgw::bridge::TdBridge& td, std::int32_t cid, HttpCallback cb) -> drogon::AsyncTask {
                auto object = co_await td.invoke(cid, td_api::make_object<td_api::getMe>());
                auto result = tgw::bridge::expect<td_api::user>(std::move(object));
                if (!result.ok()) {
                    cb(telegramError(*result.error, drogon::k502BadGateway));
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
