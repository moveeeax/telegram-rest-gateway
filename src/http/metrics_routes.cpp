#include "http/metrics_routes.hpp"

#include "auth/auth_state_manager.hpp"
#include "bridge/td_bridge.hpp"
#include "util/metrics.hpp"
#include "ws/ws_registry.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>

#include <sstream>
#include <string>

namespace tgw::http {
namespace {

// Prometheus text exposition format v0.0.4. Без внешних библиотек: метрик мало,
// рендер руками дешевле зависимости.
std::string renderMetrics(tgw::bridge::TdBridge& bridge, tgw::auth::AuthStateManager& auth) {
    auto& c = tgw::metrics::Counters::instance();
    const auto state = auth.current();

    std::ostringstream out;
    out << "# HELP tgw_ready 1 if authorized in Telegram (authorizationStateReady)\n"
        << "# TYPE tgw_ready gauge\n"
        << "tgw_ready " << (state == tgw::auth::AuthState::Ready ? 1 : 0) << "\n";

    out << "# HELP tgw_auth_state Current auth FSM state (one-hot)\n"
        << "# TYPE tgw_auth_state gauge\n"
        << "tgw_auth_state{state=\"" << tgw::auth::toString(state) << "\"} 1\n";

    out << "# HELP tgw_ws_subscribers Connected WebSocket subscribers\n"
        << "# TYPE tgw_ws_subscribers gauge\n"
        << "tgw_ws_subscribers " << tgw::ws::WsSubscriberRegistry::instance().size() << "\n";

    out << "# HELP tgw_bridge_inflight In-flight TDLib requests (correlation map size)\n"
        << "# TYPE tgw_bridge_inflight gauge\n"
        << "tgw_bridge_inflight " << bridge.pending() << "\n";

    out << "# HELP tgw_updates_forwarded_total Updates fanned out to WebSocket clients\n"
        << "# TYPE tgw_updates_forwarded_total counter\n"
        << "tgw_updates_forwarded_total " << c.updates_forwarded_total.load() << "\n";

    out << "# HELP tgw_http_requests_total HTTP requests handled\n"
        << "# TYPE tgw_http_requests_total counter\n"
        << "tgw_http_requests_total " << c.http_requests_total.load() << "\n";

    out << "# HELP tgw_http_responses_4xx_total HTTP 4xx responses\n"
        << "# TYPE tgw_http_responses_4xx_total counter\n"
        << "tgw_http_responses_4xx_total " << c.http_responses_4xx_total.load() << "\n";

    out << "# HELP tgw_http_responses_5xx_total HTTP 5xx responses\n"
        << "# TYPE tgw_http_responses_5xx_total counter\n"
        << "tgw_http_responses_5xx_total " << c.http_responses_5xx_total.load() << "\n";

    out << "# HELP tgw_ws_slow_disconnects_total WS clients dropped by back-pressure\n"
        << "# TYPE tgw_ws_slow_disconnects_total counter\n"
        << "tgw_ws_slow_disconnects_total " << c.ws_slow_disconnects_total.load() << "\n";

    out << "# HELP tgw_kafka_produced_total Events acknowledged by Kafka\n"
        << "# TYPE tgw_kafka_produced_total counter\n"
        << "tgw_kafka_produced_total " << c.kafka_produced_total.load() << "\n";

    out << "# HELP tgw_kafka_failed_total Events failed after librdkafka retries\n"
        << "# TYPE tgw_kafka_failed_total counter\n"
        << "tgw_kafka_failed_total " << c.kafka_failed_total.load() << "\n";

    out << "# HELP tgw_kafka_dropped_total Events dropped on full producer queue\n"
        << "# TYPE tgw_kafka_dropped_total counter\n"
        << "tgw_kafka_dropped_total " << c.kafka_dropped_total.load() << "\n";

    return out.str();
}

}  // namespace

void registerMetricsRoutes(tgw::bridge::TdBridge& bridge, tgw::auth::AuthStateManager& auth) {
    // Счёт HTTP-ответов: advice видит все ответы (включая 401 от фильтра и 404).
    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
            auto& c = tgw::metrics::Counters::instance();
            c.http_requests_total.fetch_add(1, std::memory_order_relaxed);
            const int code = resp->statusCode();
            if (code >= 500) {
                c.http_responses_5xx_total.fetch_add(1, std::memory_order_relaxed);
            } else if (code >= 400) {
                c.http_responses_4xx_total.fetch_add(1, std::memory_order_relaxed);
            }
        });

    drogon::app().registerHandler(
        "/metrics",
        [&bridge, &auth](const drogon::HttpRequestPtr&,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
            resp->setBody(renderMetrics(bridge, auth));
            cb(resp);
        },
        {drogon::Get});
}

}  // namespace tgw::http
