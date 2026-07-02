#include "http/bearer_filter.hpp"

#include "auth/token_store.hpp"

#include <drogon/drogon.h>

#include <string>
#include <string_view>

namespace tgw::http {

void BearerAuthFilter::doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fail,
                                drogon::FilterChainCallback&& next) {
    constexpr std::string_view kPrefix = "Bearer ";
    const std::string& header = req->getHeader("Authorization");

    if (header.size() > kPrefix.size() &&
        std::string_view(header).substr(0, kPrefix.size()) == kPrefix) {
        const std::string_view token = std::string_view(header).substr(kPrefix.size());
        if (tgw::auth::TokenStore::instance().verify(token)) {
            next();
            return;
        }
    }

    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = "UNAUTHENTICATED";
    body["error"]["message"] = "missing or invalid bearer token";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(drogon::k401Unauthorized);
    fail(resp);
}

}  // namespace tgw::http
