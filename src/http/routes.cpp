#include "http/routes.hpp"

#include "auth/auth_state_manager.hpp"
#include "auth/session_io.hpp"
#include "bridge/expect.hpp"
#include "bridge/td_bridge.hpp"
#include "http/http_helpers.hpp"
#include "http/route_table.hpp"

#include <drogon/drogon.h>
#include <drogon/RateLimiter.h>
#include <td/telegram/td_api.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace td_api = td::td_api;

namespace tgw::http {
namespace {

// jsonResponse/serviceError/telegramError/HttpCallback — общие, из http/http_helpers.hpp
// (решение 1.6). telegramError сам считает HTTP-статус из ошибки TDLib (решение 1.4).
// Пути и constraints (метод + фильтр kBearerFilter для защищённых) берутся из http/route_table.hpp
// — единого источника истины (решение ревью 3.7): здесь фильтр руками не навешивается.

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
    if (state == tgw::auth::AuthState::WaitOtherDeviceConfirmation) {
        json["qr_link"] = auth.qrLink();  // рендерить как QR; TDLib периодически обновляет
    }
    if (state == tgw::auth::AuthState::WaitCode) {
        const auto info = auth.codeInfo();
        json["code_info"]["type"] = info.type;            // куда отправлен код
        json["code_info"]["next_type"] = info.next_type;  // тип после resend
        json["code_info"]["length"] = info.length;
        json["code_info"]["resend_timeout"] = info.timeout;
    }
    return json;
}

// Запускает detached-корутину для auth-мутации: co_await ответа, ok -> текущее состояние,
// error -> 400 с проброшенным td-кодом. registerHandler не биндит корутинные лямбды,
// поэтому корутину гоняем внутри обычного callback-хендлера (см. §5.4 и историю /v1/me).
// Про «fair auth-mutex» (бэклог): отдельный мьютекс не нужен. TDLib обрабатывает запросы
// одного клиента строго в порядке отправки (один поток-приёмник, send сериализован), а
// конкурирующие мутации FSM дают детерминированную ошибку TDLib -> 400/409 клиенту.
void launchAuthMutation(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                        tgw::auth::AuthStateManager& auth, td_api::object_ptr<td_api::Function> fn,
                        HttpCallback callback) {
    [](tgw::bridge::TdBridge& td, std::int32_t cid, tgw::auth::AuthStateManager& a,
       td_api::object_ptr<td_api::Function> f, HttpCallback cb) -> drogon::AsyncTask {
        auto object = co_await td.invoke(cid, std::move(f));
        if (object != nullptr && object->get_id() == td_api::error::ID) {
            cb(telegramError(static_cast<td_api::error&>(*object)));
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
// isObject() обязателен ПЕРЕД isMember/operator[]: тело вида `[]` или `5` тоже разбирается
// jsoncpp успешно, а isMember/operator[] на не-объекте кидают Json::LogicError.
bool jsonString(const drogon::HttpRequestPtr& req, const char* field, std::string& out,
                std::string& err) {
    auto json = req->getJsonObject();
    if (json == nullptr || !json->isObject() || !json->isMember(field) ||
        !(*json)[field].isString()) {
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

// Anti-bruteforce на секретные auth-мутации (§7.x): скользящее окно 10 попыток/мин на процесс
// (single-account — per-IP не нужен, сервис за периметром). SafeRateLimiter — потокобезопасен.
bool authAttemptAllowed() {
    static const drogon::RateLimiterPtr limiter =
        std::make_shared<drogon::SafeRateLimiter>(drogon::RateLimiter::newRateLimiter(
            drogon::RateLimiterType::kSlidingWindow, 10, std::chrono::seconds(60)));
    return limiter->isAllowed();
}

void registerRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                    tgw::auth::AuthStateManager& auth, const std::string& database_dir) {
    auto& app = drogon::app();

    // Регистрация строго через таблицу маршрутов (http/route_table.hpp): путь и constraints
    // (метод + фильтр kBearerFilter для защищённых) берутся из RouteSpec, поэтому фильтр здесь
    // руками не навешивается и «забыть» его нельзя (решение ревью 3.7).
    auto registerRoute = [&app](const RouteSpec& spec, auto&& handler) {
        app.registerHandler(std::string(spec.path), std::forward<decltype(handler)>(handler),
                            constraintsFor(spec));
    };

    // --- system (без auth) ---
    registerRoute(kHealthRoute, [](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
        Json::Value body;
        body["ok"] = true;
        body["status"] = "alive";
        cb(drogon::HttpResponse::newHttpJsonResponse(body));
    });

    registerRoute(kReadyRoute, [&auth](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
        const bool ready = (auth.current() == tgw::auth::AuthState::Ready);
        Json::Value body;
        body["ready"] = ready;
        body["state"] = std::string(tgw::auth::toString(auth.current()));
        cb(jsonResponse(std::move(body), ready ? drogon::k200OK : drogon::k503ServiceUnavailable));
    });

    // --- auth (§7.2) — single-account, session_id неявно "default" ---
    // GET /v1/auth/session/export — «session string» (base64 td.binlog) для stateless-запуска
    // через TGW_SESSION. Содержит auth key: полноценный доступ к аккаунту — хранить как секрет.
    registerRoute(
        kAuthSessionExportRoute, [database_dir](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
            const auto session = tgw::auth::exportSession(database_dir);
            if (!session) {
                cb(serviceError("NOT_FOUND", "session binlog not found (not logged in yet?)",
                                drogon::k404NotFound));
                return;
            }
            Json::Value body;
            body["ok"] = true;
            body["data"]["session_b64"] = *session;
            body["data"]["note"] = "store as secret; pass via TGW_SESSION on next start";
            cb(jsonResponse(std::move(body), drogon::k200OK));
        });

    registerRoute(kAuthStateRoute, [&auth](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
        Json::Value body;
        body["ok"] = true;
        body["data"] = authStateJson(auth);
        cb(jsonResponse(std::move(body), drogon::k200OK));
    });

    // setTdlibParameters уже отправлен StartupBootstrapper'ом; эндпоинт идемпотентен —
    // просто возвращает текущее состояние (§7.1).
    registerRoute(kAuthSessionRoute, [&auth](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
        Json::Value body;
        body["ok"] = true;
        body["data"] = authStateJson(auth);
        cb(jsonResponse(std::move(body), drogon::k200OK));
    });

    registerRoute(kAuthPhoneRoute, [&bridge, client_id, &auth](const drogon::HttpRequestPtr& req,
                                                               HttpCallback&& cb) {
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
    });

    // POST /v1/auth/qr — переключить логин на QR (requestQrCodeAuthentication). Ссылка
    // появится в /v1/auth/state (qr_link); сканировать с телефона: Настройки -> Устройства.
    registerRoute(kAuthQrRoute,
                  [&bridge, client_id, &auth](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
                      auto fn = td_api::make_object<td_api::requestQrCodeAuthentication>();
                      launchAuthMutation(bridge, client_id, auth, std::move(fn), std::move(cb));
                  });

    // POST /v1/auth/code/resend — переотправить код (после resend_timeout переключает на
    // next_type, обычно SMS). resendCodeReasonUserRequest.
    registerRoute(kAuthCodeResendRoute,
                  [&bridge, client_id, &auth](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
                      auto fn = td_api::make_object<td_api::resendAuthenticationCode>();
                      fn->reason_ = td_api::make_object<td_api::resendCodeReasonUserRequest>();
                      launchAuthMutation(bridge, client_id, auth, std::move(fn), std::move(cb));
                  });

    registerRoute(kAuthCodeRoute, [&bridge, client_id, &auth](const drogon::HttpRequestPtr& req,
                                                              HttpCallback&& cb) {
        if (!authAttemptAllowed()) {
            cb(serviceError("FLOOD_WAIT", "too many auth attempts, retry later",
                            drogon::k429TooManyRequests));
            return;
        }
        std::string code;
        std::string err;
        if (!jsonString(req, "code", code, err)) {
            cb(serviceError("VALIDATION_ERROR", err, drogon::k400BadRequest));
            return;
        }
        launchAuthMutation(bridge, client_id, auth,
                           td_api::make_object<td_api::checkAuthenticationCode>(code),
                           std::move(cb));
    });

    registerRoute(kAuthPasswordRoute, [&bridge, client_id, &auth](const drogon::HttpRequestPtr& req,
                                                                  HttpCallback&& cb) {
        if (!authAttemptAllowed()) {
            cb(serviceError("FLOOD_WAIT", "too many auth attempts, retry later",
                            drogon::k429TooManyRequests));
            return;
        }
        std::string password;
        std::string err;
        if (!jsonString(req, "password", password, err)) {
            cb(serviceError("VALIDATION_ERROR", err, drogon::k400BadRequest));
            return;
        }
        launchAuthMutation(bridge, client_id, auth,
                           td_api::make_object<td_api::checkAuthenticationPassword>(password),
                           std::move(cb));
    });

    // --- GET /v1/me (мост, §12 этап 1) ---
    registerRoute(
        kMeRoute, [&bridge, client_id](const drogon::HttpRequestPtr&, HttpCallback&& callback) {
            [](tgw::bridge::TdBridge& td, std::int32_t cid, HttpCallback cb) -> drogon::AsyncTask {
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
        });
}

}  // namespace tgw::http
