#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>
#include <td/telegram/td_api.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "bridge/real_transport.hpp"
#include "bridge/td_bridge.hpp"
#include "bridge/update_sink.hpp"
#include "http/routes.hpp"

namespace td_api = td::td_api;

namespace {

constexpr std::uint16_t kDefaultPort = 8080;

// Подкоманда для distroless HEALTHCHECK (нет shell/curl): локальный GET /v1/health.
int runHealthcheck(std::uint16_t port) {
    auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(port));
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/v1/health");
    auto [result, resp] = client->sendRequest(req, 3.0);
    if (result == drogon::ReqResult::Ok && resp && resp->statusCode() == drogon::k200OK) {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}

std::uint16_t listenPort() {
    if (const char* env = std::getenv("TGW_LISTEN_PORT")) {
        return static_cast<std::uint16_t>(std::atoi(env));
    }
    return kDefaultPort;
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint16_t port = listenPort();

    if (argc > 1 && std::string_view(argv[1]) == "--healthcheck") {
        return runHealthcheck(port);
    }

    const char* addr_env = std::getenv("TGW_LISTEN_ADDRESS");
    const std::string listen_address = addr_env ? addr_env : "127.0.0.1";

    // --- Поднимаем мост (этап 1). Полная авторизация/шифрование БД — этап 2. ---
    tgw::bridge::RealTdTransport transport;
    tgw::bridge::CountingUpdateSink update_sink;
    tgw::bridge::TdBridge bridge(transport, update_sink, tgw::bridge::BridgeConfig{});

    const std::int32_t client_id = bridge.createClientId();
    bridge.start();

    // Первый запрос инициирует поток updateAuthorizationState (§7.1).
    // На этапе 1 — просто «толчок»; setTdlibParameters придёт на этапе 2 (StartupBootstrapper).
    bridge.sendOneWay(client_id, td_api::make_object<td_api::getOption>("version"));

    tgw::http::registerRoutes(bridge, client_id);

    // Корректная остановка моста при выходе из run() (полный graceful shutdown — §10.4, этап 6).
    drogon::app().registerBeginningAdvice([&]() {
        LOG_INFO << "telegram-rest-gateway listening on " << listen_address << ":" << port;
    });

    drogon::app().addListener(listen_address, port).run();

    bridge.stop();
    return EXIT_SUCCESS;
}
