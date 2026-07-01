#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

constexpr std::uint16_t kDefaultPort = 8080;

// Liveness-эндпоинт (§8.9): всегда 200, если процесс жив, без авторизации.
// Stage 0 — только каркас; /v1/ready, авторизация, мост появятся на этапах 1–2.
void registerHealthRoute() {
    drogon::app().registerHandler(
        "/v1/health",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            Json::Value body;
            body["ok"] = true;
            body["status"] = "alive";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
            callback(resp);
        },
        {drogon::Get});
}

// Подкоманда для distroless HEALTHCHECK (нет shell/curl): локальный GET /v1/health.
int runHealthcheck(std::uint16_t port) {
    auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(port));
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/v1/health");
    auto [result, resp] = client->sendRequest(req, 3.0);
    if (result == drogon::ReqResult::Ok && resp &&
        resp->statusCode() == drogon::k200OK) {
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

    // По умолчанию слушаем loopback (§8.0): наружу — только через TLS-reverse-proxy.
    const char* addr_env = std::getenv("TGW_LISTEN_ADDRESS");
    const std::string listen_address = addr_env ? addr_env : "127.0.0.1";

    registerHealthRoute();

    LOG_INFO << "telegram-rest-gateway starting on " << listen_address << ":" << port;
    drogon::app().addListener(listen_address, port).run();
    return EXIT_SUCCESS;
}
