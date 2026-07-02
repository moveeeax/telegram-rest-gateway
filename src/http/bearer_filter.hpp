#pragma once

#include <drogon/HttpFilter.h>

namespace tgw::http {

// Аутентификация API-клиента статическим Bearer-токеном (§8.1). Вешается на все /v1/**
// кроме /v1/health и /v1/ready. Токены читаются из TokenStore (заполняется в main).
class BearerAuthFilter : public drogon::HttpFilter<BearerAuthFilter> {
   public:
    void doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fail,
                  drogon::FilterChainCallback&& next) override;
};

}  // namespace tgw::http
