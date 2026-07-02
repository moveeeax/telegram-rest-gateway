#include "auth/auth_state_manager.hpp"
#include "auth/startup_bootstrapper.hpp"
#include "auth/token_store.hpp"
#include "bridge/real_transport.hpp"
#include "bridge/td_bridge.hpp"
#include "config/config.hpp"
#include "http/routes.hpp"
#include "ws/update_router.hpp"

#include <drogon/drogon.h>
#include <drogon/HttpAppFramework.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr std::uint16_t kDefaultPort = 8080;

// Подкоманда для distroless HEALTHCHECK (нет shell/curl): локальный GET /v1/health.
// Секретов не требует (Config не грузим), чтобы HEALTHCHECK работал всегда.
int runHealthcheck() {
    std::uint16_t port = kDefaultPort;
    if (const char* env = std::getenv("TGW_LISTEN_PORT")) {
        port = static_cast<std::uint16_t>(std::atoi(env));
    }
    auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(port));
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/v1/health");
    auto [result, resp] = client->sendRequest(req, 3.0);
    if (result == drogon::ReqResult::Ok && resp != nullptr &&
        resp->statusCode() == drogon::k200OK) {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--healthcheck") {
        return runHealthcheck();
    }

    tgw::config::Config config;
    try {
        config = tgw::config::Config::load();
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    // API-токены клиентов (§8.1). Пусто — предупреждаем: сервис fail-closed (все 401).
    tgw::auth::TokenStore::instance().load(config.bearer_tokens);
    if (tgw::auth::TokenStore::instance().empty()) {
        LOG_WARN << "no BEARER_TOKENS configured: all protected endpoints will return 401";
    }

    // Мост + приёмник апдейтов = AuthStateManager (обрабатывает updateAuthorizationState).
    tgw::bridge::RealTdTransport transport;
    tgw::auth::AuthStateManager auth;
    tgw::ws::UpdateRouter router(auth);  // авторизационные -> auth, прикладные -> WS fan-out
    tgw::bridge::TdBridge bridge(transport, router, tgw::bridge::BridgeConfig{});

    const std::int32_t client_id = bridge.createClientId();
    bridge.start();

    // Автоподъём сессии ДО приёма HTTP (§7.1): глушим лог, шлём setTdlibParameters.
    tgw::auth::StartupBootstrapper::run(bridge, client_id, config, auth);

    const std::string upload_dir = config.files_directory + "/uploads";
    std::error_code mkdir_ec;
    std::filesystem::create_directories(upload_dir, mkdir_ec);
    drogon::app().setClientMaxBodySize(config.max_upload_bytes);

    tgw::http::registerRoutes(bridge, client_id, auth);
    tgw::http::registerMessageRoutes(bridge, client_id, upload_dir);

    LOG_INFO << "telegram-rest-gateway listening on " << config.listen_address << ":"
             << config.listen_port;
    drogon::app().addListener(config.listen_address, config.listen_port).run();

    bridge.stop();
    return EXIT_SUCCESS;
}
