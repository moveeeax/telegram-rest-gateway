#pragma once

#include "webhook/webhook_registry.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace tgw::http {

// REST-роуты управления реестром вебхуков mention/reply (Task 7): POST/GET /v1/webhooks,
// DELETE /v1/webhooks/{id}. Все требуют admin scope (см. http/scope_policy.hpp) — пути и
// constraints берутся из http/route_table.hpp (единый источник истины, решение ревью 3.7).
// WebhookRegistry синхронный (add/remove/list не ходят в сеть под локом — см.
// webhook_registry.hpp), поэтому, в отличие от message_routes/directory_routes, хендлерам не нужна
// TDLib-корутина.
void registerWebhookRoutes(tgw::webhook::WebhookRegistry& registry);

// POST /v1/webhooks — вынесен в отдельную функцию ради юнит-теста без Drogon-роутера (тот же
// подход, что runFilter в tests/route_smoke_test.cpp): валидирует и разбирает тело, на успехе
// зовёт registry.add(...). registerWebhookRoutes оборачивает её в обычный HttpCallback-хендлер.
drogon::HttpResponsePtr handleWebhookCreate(const drogon::HttpRequestPtr& req,
                                            tgw::webhook::WebhookRegistry& registry);

}  // namespace tgw::http
