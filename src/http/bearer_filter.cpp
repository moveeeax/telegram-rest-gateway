#include "http/bearer_filter.hpp"

#include "auth/token_store.hpp"
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
    constexpr std::string_view kPrefix = "Bearer ";
    const std::string& header = req->getHeader("Authorization");

    if (header.size() > kPrefix.size() &&
        std::string_view(header).substr(0, kPrefix.size()) == kPrefix) {
        const std::string_view token = std::string_view(header).substr(kPrefix.size());
        const auto scopes = tgw::auth::TokenStore::instance().verify(token);
        if (scopes.has_value()) {
            if (tgw::auth::scopeAllows(*scopes, requiredScope(req))) {
                next();
                return;
            }
            fail(serviceError("INSUFFICIENT_SCOPE",
                              "token lacks the scope required for this endpoint",
                              drogon::k403Forbidden));
            return;
        }
    }

    fail(serviceError("UNAUTHENTICATED", "missing or invalid bearer token",
                      drogon::k401Unauthorized));
}

}  // namespace tgw::http
