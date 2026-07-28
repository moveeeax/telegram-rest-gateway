#include "auth/token_store.hpp"
#include "http/bearer_filter.hpp"
#include "http/http_helpers.hpp"
#include "http/route_table.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <td/telegram/td_api.h>

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace td_api = td::td_api;

// ЗАМЕЧАНИЕ О ПОДХОДЕ (пункт 3.7, после ревью).
// Целевая регрессия — «добавили /v1-роут в routes.cpp и забыли навесить {kBearerFilter}» —
// закрыта НЕ этим тестом, а КОНСТРУКЦИЕЙ: registerRoutes регистрирует пути и constraints из
// http/route_table.hpp (единый источник истины), поле requires_auth автоматически добавляет
// фильтр, строка kBearerFilter в routes.cpp больше не пишется руками. Забыть фильтр там нельзя.
// Тест ниже итерирует ту же таблицу kRoutesTable и проверяет, что логика фильтра
// (requiredScopeFor в BearerAuthFilter) СОГЛАСОВАНА с таблицей: каждый requires_auth-путь без
// токена настоящий фильтр отклоняет 401. Это ловит расхождение таблицы и логики фильтра; это НЕ
// проверка факта навешивания (его гарантирует конструкция) и не запуск сети.
//
// Живой drogon::app().run() не поднимается сознательно: CI собирает и гоняет под ASan/TSan только
// tgw_unit_tests (--target tgw_unit_tests), а поднимать сервер в общем тест-процессе под TSan (где
// tsan.supp покрывает лишь libcrypto/libssl) хрупко и невозможно верифицировать в этой среде.
//
// ОБЛАСТЬ: таблица и этот тест покрывают маршруты routes.cpp (системные + вся auth-поверхность +
// /v1/me). Ресурсные маршруты message_routes.cpp/directory_routes.cpp регистрируются прежним
// способом и в таблицу НЕ включены (иначе тест давал бы по ним ложно-зелёный результат) — их
// миграция в таблицу отдельный follow-up.

namespace {

using tgw::auth::TokenStore;
using tgw::http::BearerAuthFilter;

struct FilterOutcome {
    bool next_called = false;
    bool fail_called = false;
    drogon::HttpStatusCode status = drogon::k200OK;
};

// Прогоняет реальный BearerAuthFilter на синтетическом запросе (без сети). Наш doFilter вызывает
// fail()/next() синхронно, поэтому исход готов сразу после возврата.
FilterOutcome runFilter(const std::string& path, drogon::HttpMethod method,
                        const std::string& authorization) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(path);
    if (!authorization.empty()) {
        req->addHeader("Authorization", authorization);
    }

    FilterOutcome outcome;
    BearerAuthFilter filter;
    filter.doFilter(
        req,
        [&outcome](const drogon::HttpResponsePtr& resp) {
            outcome.fail_called = true;
            outcome.status = resp->statusCode();
        },
        [&outcome]() { outcome.next_called = true; });
    return outcome;
}

}  // namespace

// Согласованность таблицы маршрутов и логики фильтра: каждый защищённый путь из kRoutesTable без
// токена отклоняется 401 настоящим BearerAuthFilter; публичные — ровно /v1/health и /v1/ready.
TEST(RouteSmoke, TableRoutesAgreeWithFilter) {
    TokenStore::instance().load({});  // пустой стор -> ни один токен не валиден

    std::vector<std::string> public_paths;
    for (const auto& route : tgw::http::kRoutesTable) {
        if (!route.requires_auth) {
            // Публичный маршрут: по таблице фильтр не навешивается (набор проверяем ниже).
            public_paths.emplace_back(route.path);
            continue;
        }
        const auto outcome = runFilter(std::string(route.path), route.method, /*authorization=*/"");
        EXPECT_TRUE(outcome.fail_called) << route.path;
        EXPECT_FALSE(outcome.next_called) << route.path;
        EXPECT_EQ(outcome.status, drogon::k401Unauthorized) << route.path;
    }

    // Незащищённые маршруты routes.cpp — ровно /v1/health и /v1/ready (bit-for-bit как в
    // регистрации). Формулировка ревью «/v1/health — единственный незащищённый» неточна:
    // /v1/ready тоже открыт, и это сохранено намеренно.
    EXPECT_EQ(public_paths, (std::vector<std::string>{"/v1/health", "/v1/ready"}));
}

// Разграничение скоупов через настоящий фильтр: read-токен на admin-пути -> 403, admin-токен ->
// проход; read-токена достаточно для GET-маршрута. Пути берём из таблицы.
TEST(RouteSmoke, ScopeEnforcementThroughRealFilter) {
    TokenStore::instance().load({"tgw_ro read", "tgw_admin"});

    const std::string admin_path{tgw::http::kAuthSessionExportRoute.path};
    const std::string me_path{tgw::http::kMeRoute.path};

    const auto ro_on_admin =
        runFilter(admin_path, tgw::http::kAuthSessionExportRoute.method, "Bearer tgw_ro");
    EXPECT_TRUE(ro_on_admin.fail_called);
    EXPECT_EQ(ro_on_admin.status, drogon::k403Forbidden);

    const auto admin_on_admin =
        runFilter(admin_path, tgw::http::kAuthSessionExportRoute.method, "Bearer tgw_admin");
    EXPECT_TRUE(admin_on_admin.next_called);
    EXPECT_FALSE(admin_on_admin.fail_called);

    const auto ro_on_read = runFilter(me_path, tgw::http::kMeRoute.method, "Bearer tgw_ro");
    EXPECT_TRUE(ro_on_read.next_called);
    EXPECT_FALSE(ro_on_read.fail_called);
}

// (в) Маппинг ошибок TDLib (решение 1.4), через который проходит ответ каждого моста-маршрута:
// 400 "Chat not found" -> 404; иной 400 -> 400; не-4xx проблемы гейтвея -> 502.
TEST(RouteSmoke, TelegramErrorMappingChatNotFoundIs404) {
    auto not_found = td_api::make_object<td_api::error>(400, "Chat not found");
    EXPECT_EQ(tgw::http::httpStatusForTdError(*not_found), drogon::k404NotFound);
    auto resp = tgw::http::telegramError(*not_found);
    EXPECT_EQ(resp->statusCode(), drogon::k404NotFound);

    auto bad_request = td_api::make_object<td_api::error>(400, "PHONE_NUMBER_INVALID");
    EXPECT_EQ(tgw::http::httpStatusForTdError(*bad_request), drogon::k400BadRequest);

    auto internal = td_api::make_object<td_api::error>(500, "Internal server error");
    EXPECT_EQ(tgw::http::httpStatusForTdError(*internal), drogon::k502BadGateway);
}
