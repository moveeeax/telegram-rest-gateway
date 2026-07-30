#pragma once

#include <drogon/HttpTypes.h>
#include <drogon/utils/HttpConstraint.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

// Единый источник истины по маршрутам registerRoutes (routes.cpp) — решение ревью Task 3,
// находка 3.7. Здесь объявляется каждый такой /v1-маршрут ровно один раз: путь, HTTP-метод и
// признак «требует Bearer-аутентификации». registerRoutes регистрирует хендлеры, беря И путь,
// И список constraints ИЗ этой таблицы (через registerRoute/constraintsFor) — строка kBearerFilter
// в routes.cpp больше не пишется руками. Поэтому «добавили /v1-роут в routes.cpp и забыли навесить
// фильтр» невозможно по конструкции: защищённость задаётся полем requires_auth ровно здесь, а
// фильтр добавляется автоматически. Тест tests/route_smoke_test.cpp итерирует ту же таблицу
// (kRoutesTable): для каждого защищённого пути настоящий BearerAuthFilter обязан вернуть 401 без
// токена — так ловится расхождение таблицы и логики requiredScopeFor.
//
// ОБЛАСТЬ ДЕЙСТВИЯ (сознательно ограничена): таблица покрывает маршруты routes.cpp — системные
// (/v1/health, /v1/ready) и всю аутентификационную поверхность (/v1/auth/*, /v1/me), т.е. самую
// критичную по безопасности (session export = захват аккаунта), а также /v1/webhooks* —
// регистрацию вебхуков mention/reply-событий (webhook_routes.cpp, Task 7): тоже admin-only,
// т.к. список/секреты вебхуков определяют, куда утечёт содержимое чужих сообщений. Ресурсные
// маршруты message_routes.cpp/directory_routes.cpp регистрируются в своих файлах прежним
// способом и в эту таблицу НЕ включены — иначе тест давал бы по ним ложно-зелёный результат
// (проверял бы логику фильтра, а не факт его навешивания там). Их миграция в таблицу —
// механический follow-up.
//
// ВАЖНО: имя фильтра = полное имя класса BearerAuthFilter (регистрируется по нему через
// HttpFilter<>). Обязано совпадать с классом в http/bearer_filter.hpp.
namespace tgw::http {

inline constexpr std::string_view kBearerFilterName = "tgw::http::BearerAuthFilter";

// Описание одного зарегистрированного маршрута. Литеральный тип -> можно объявлять constexpr.
struct RouteSpec {
    std::string_view path;  // паттерн пути Drogon (может содержать {placeholders})
    drogon::HttpMethod method;  // ровно один метод на запись (как в registerHandler)
    bool requires_auth;  // true -> в constraints добавляется kBearerFilter
};

// Строит вектор constraints для registerHandler из спецификации. Порядок метод/фильтр не важен:
// registerHandler раскладывает constraints по типам (методы и middleware — раздельно), поэтому
// результат бит-в-бит эквивалентен прежним литералам {method} / {method, kBearerFilter}.
inline std::vector<drogon::internal::HttpConstraint> constraintsFor(const RouteSpec& spec) {
    std::vector<drogon::internal::HttpConstraint> constraints;
    constraints.emplace_back(spec.method);
    if (spec.requires_auth) {
        constraints.emplace_back(std::string(kBearerFilterName));
    }
    return constraints;
}

// --- Системные (без аутентификации) ---
inline constexpr RouteSpec kHealthRoute{"/v1/health", drogon::Get, false};
inline constexpr RouteSpec kReadyRoute{"/v1/ready", drogon::Get, false};

// --- Аутентификация/сессия и /v1/me (все требуют токен) ---
inline constexpr RouteSpec kAuthSessionExportRoute{"/v1/auth/session/export", drogon::Get, true};
inline constexpr RouteSpec kAuthStateRoute{"/v1/auth/state", drogon::Get, true};
inline constexpr RouteSpec kAuthSessionRoute{"/v1/auth/session", drogon::Post, true};
inline constexpr RouteSpec kAuthPhoneRoute{"/v1/auth/phone", drogon::Post, true};
inline constexpr RouteSpec kAuthQrRoute{"/v1/auth/qr", drogon::Post, true};
inline constexpr RouteSpec kAuthCodeResendRoute{"/v1/auth/code/resend", drogon::Post, true};
inline constexpr RouteSpec kAuthCodeRoute{"/v1/auth/code", drogon::Post, true};
inline constexpr RouteSpec kAuthPasswordRoute{"/v1/auth/password", drogon::Post, true};
inline constexpr RouteSpec kMeRoute{"/v1/me", drogon::Get, true};

// --- Вебхуки mention/reply (webhook_routes.cpp, Task 7) — все требуют токен, scope admin (см.
// http/scope_policy.hpp: requiredScopeFor узнаёт префикс /v1/webhooks так же, как /v1/auth/*). ---
inline constexpr RouteSpec kWebhookCreateRoute{"/v1/webhooks", drogon::Post, true};
inline constexpr RouteSpec kWebhookListRoute{"/v1/webhooks", drogon::Get, true};
inline constexpr RouteSpec kWebhookDeleteRoute{"/v1/webhooks/{id}", drogon::Delete, true};

// Полный перечень маршрутов routes.cpp + webhook_routes.cpp для итерации тестом. Значения — те
// же спецификации, что выше: по ним же регистрируются хендлеры, поэтому таблица и регистрация
// не расходятся.
inline constexpr std::array<RouteSpec, 14> kRoutesTable{{
    kHealthRoute,
    kReadyRoute,
    kAuthSessionExportRoute,
    kAuthStateRoute,
    kAuthSessionRoute,
    kAuthPhoneRoute,
    kAuthQrRoute,
    kAuthCodeResendRoute,
    kAuthCodeRoute,
    kAuthPasswordRoute,
    kMeRoute,
    kWebhookCreateRoute,
    kWebhookListRoute,
    kWebhookDeleteRoute,
}};

}  // namespace tgw::http
