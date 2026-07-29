#include "http/bearer_filter.hpp"

#include "auth/token_store.hpp"
#include "http/bearer_auth.hpp"
#include "http/http_helpers.hpp"
#include "http/scope_policy.hpp"

#include <drogon/drogon.h>

#include <string>
#include <string_view>

namespace tgw::http {
namespace {

// Политика скоупов живёт в http/scope_policy.hpp (чистая функция, покрыта юнит-тестом).
// Здесь — только извлечение пути и метода из запроса. Конверт ошибки — общий serviceError
// из http/http_helpers.hpp (решение 1.6: раньше был локальный errorResponse с иной сигнатурой).
tgw::auth::Scope requiredScope(const drogon::HttpRequestPtr& req) {
    const bool is_read_method = (req->method() == drogon::Get || req->method() == drogon::Head);
    return requiredScopeFor(req->path(), is_read_method);
}

}  // namespace

void BearerAuthFilter::doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fail,
                                drogon::FilterChainCallback&& next) {
    // Разбор заголовка и решение 401-vs-403 — в http/bearer_auth.hpp (чистая логика, покрыта
    // tests/bearer_auth_test.cpp). Здесь остаётся только маршалинг req -> исход -> HTTP-ответ.
    const std::string& header = req->getHeader("Authorization");
    const BearerAuthResult result = evaluateBearerAuth(
        header, requiredScope(req),
        [](std::string_view token) { return tgw::auth::TokenStore::instance().verify(token); });

    switch (result) {
        case BearerAuthResult::Allowed:
            next();
            return;
        case BearerAuthResult::Forbidden:
            fail(serviceError("INSUFFICIENT_SCOPE",
                              "token lacks the scope required for this endpoint",
                              drogon::k403Forbidden));
            return;
        case BearerAuthResult::Unauthenticated:
            fail(serviceError("UNAUTHENTICATED", "missing or invalid bearer token",
                              drogon::k401Unauthorized));
            return;
    }
}

}  // namespace tgw::http
