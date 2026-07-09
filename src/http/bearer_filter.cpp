#include "http/bearer_filter.hpp"

#include "auth/token_store.hpp"

#include <drogon/drogon.h>

#include <string>
#include <string_view>

namespace tgw::http {
namespace {

// Требуемый скоуп по маршруту: /v1/auth/* — admin (логин, session export = захват аккаунта);
// GET/HEAD — read; остальные методы (мутации) — write.
tgw::auth::Scope requiredScope(const drogon::HttpRequestPtr& req) {
    const std::string& path = req->path();
    if (path.rfind("/v1/auth/", 0) == 0) {
        return tgw::auth::Scope::Admin;
    }
    if (req->method() == drogon::Get || req->method() == drogon::Head) {
        return tgw::auth::Scope::Read;
    }
    return tgw::auth::Scope::Write;
}

drogon::HttpResponsePtr errorResponse(const char* code, const char* message,
                                      drogon::HttpStatusCode status) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = code;
    body["error"]["message"] = message;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(status);
    return resp;
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
            fail(errorResponse("INSUFFICIENT_SCOPE",
                               "token lacks the scope required for this endpoint",
                               drogon::k403Forbidden));
            return;
        }
    }

    fail(errorResponse("UNAUTHENTICATED", "missing or invalid bearer token",
                       drogon::k401Unauthorized));
}

}  // namespace tgw::http
