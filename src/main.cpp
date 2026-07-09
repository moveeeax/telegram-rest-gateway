#include "auth/auth_state_manager.hpp"
#include "auth/s3_session.hpp"
#include "auth/session_io.hpp"
#include "auth/startup_bootstrapper.hpp"
#include "auth/token_store.hpp"
#include "bridge/real_transport.hpp"
#include "bridge/td_bridge.hpp"
#include "config/config.hpp"
#include "events/kafka_sink.hpp"
#include "http/directory_routes.hpp"
#include "http/login_ui.hpp"
#include "http/metrics_routes.hpp"
#include "http/routes.hpp"
#include "http/upload_cleanup.hpp"
#include "ws/update_router.hpp"
#include "ws/updates_ws.hpp"
#include "ws/ws_registry.hpp"

#include <drogon/drogon.h>
#include <drogon/HttpAppFramework.h>
#include <td/telegram/td_api.h>
#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace td_api = td::td_api;

namespace {

constexpr std::uint16_t kDefaultPort = 8080;

// Подкоманда для distroless HEALTHCHECK (нет shell/curl): локальный GET /v1/health.
// Секретов не требует (Config не грузим), чтобы HEALTHCHECK работал всегда.
int runHealthcheck() {
    std::uint16_t port = kDefaultPort;
    if (const char* env = std::getenv("TGW_LISTEN_PORT")) {
        port = static_cast<std::uint16_t>(std::atoi(env));
    }
    // Здесь app().run() не вызывается, главного event-loop нет — синхронный sendRequest завис бы
    // навсегда (и HEALTHCHECK стабильно валился по таймауту). Крутим запрос на собственном
    // коротком EventLoopThread — тот же паттерн, что в util/s3_client.
    trantor::EventLoopThread loop_thread;
    loop_thread.run();
    auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(port),
                                                    loop_thread.getLoop());
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/v1/health");

    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    client->sendRequest(
        req,
        [promise](drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
            promise->set_value(result == drogon::ReqResult::Ok && resp != nullptr &&
                               resp->statusCode() == drogon::k200OK);
        },
        2.0);

    bool ok = false;
    if (future.wait_for(std::chrono::milliseconds(2500)) == std::future_status::ready) {
        ok = future.get();
    }
    loop_thread.getLoop()->quit();
    loop_thread.wait();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
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

    // Глушим лог TDLib СИНХРОННО до любой активности клиента (§13), иначе стартовый поток
    // подробных логов проскакивает в stdout.
    tgw::bridge::RealTdTransport::configureGlobalLogging(config.tdlib_log_verbosity);

    // API-токены клиентов (§8.1). Пусто — предупреждаем: сервис fail-closed (все 401).
    tgw::auth::TokenStore::instance().load(config.bearer_tokens);
    if (tgw::auth::TokenStore::instance().empty()) {
        LOG_WARN << "no BEARER_TOKENS configured: all protected endpoints will return 401";
    }

    // S3-хранилище сессии: если сконфигурировано и локального binlog нет — тянем из S3 ДО
    // создания клиента (§stateless). Пишет td.binlog, дальше restoreSession увидит existing.
    switch (tgw::auth::restoreFromS3(config.s3, config.database_directory)) {
        case tgw::auth::S3RestoreResult::Restored:
            LOG_INFO << "session restored from S3 (" << config.s3.bucket << "/" << config.s3.key
                     << ")";
            break;
        case tgw::auth::S3RestoreResult::NoRemoteObject:
            LOG_INFO << "no session object in S3 — fresh start (login via /v1/auth/*)";
            break;
        case tgw::auth::S3RestoreResult::SkippedExisting:
            LOG_WARN << "S3 configured, but local td.binlog exists — keeping it";
            break;
        case tgw::auth::S3RestoreResult::Error:
            std::cerr << "failed to restore session from S3\n";
            return EXIT_FAILURE;
        case tgw::auth::S3RestoreResult::NotConfigured:
            break;  // S3 не используется — обычный путь
    }

    // Stateless: восстановить сессию из TGW_SESSION (base64 td.binlog) ДО создания клиента.
    switch (tgw::auth::restoreSession(config.database_directory, config.session_b64)) {
        case tgw::auth::RestoreResult::Restored:
            LOG_INFO << "session restored from TGW_SESSION";
            break;
        case tgw::auth::RestoreResult::SkippedExisting:
            LOG_WARN << "TGW_SESSION set, but td.binlog already exists — keeping existing session";
            break;
        case tgw::auth::RestoreResult::Error:
            std::cerr << "invalid TGW_SESSION (bad base64 or write failure)\n";
            return EXIT_FAILURE;
        case tgw::auth::RestoreResult::NoSession:
            break;  // чистый старт — логин через /v1/auth/*
    }

    // Kafka-канал событий (no-op если TGW_KAFKA_BROKERS пуст).
    auto kafka = tgw::events::KafkaSink::create(config.kafka);

    // Мост + приёмник апдейтов = AuthStateManager (обрабатывает updateAuthorizationState).
    tgw::bridge::RealTdTransport transport;
    tgw::auth::AuthStateManager auth;
    // авторизационные -> auth, прикладные -> WS fan-out (+ Kafka, если включена)
    tgw::ws::UpdateRouter router(auth, config.session_id);
    if (kafka) {
        router.setEventPublisher(
            [sink = kafka.get()](const std::string& key, const std::string& payload) {
                sink->produce(key, payload);
            });
    }
    tgw::bridge::TdBridge bridge(transport, router, tgw::bridge::BridgeConfig{});

    const std::int32_t client_id = bridge.createClientId();

    // Колбэки регистрируем ДО start(): восстановленная из S3/TGW_SESSION сессия доходит до
    // Ready ещё в StartupBootstrapper — регистрация после него теряет событие.
    // Warmup чатов (§бэклог ChatCache): TDLib наполняет главный список лениво — без прогрева
    // первый GET /v1/chats после старта может отдать частичный список. Fire-and-forget.
    auth.setOnReady([&bridge, client_id] {
        auto load = td_api::make_object<td_api::loadChats>();
        load->chat_list_ = td_api::make_object<td_api::chatListMain>();
        load->limit_ = 100;
        bridge.sendOneWay(client_id, std::move(load));
        LOG_INFO << "authorized: warming up main chat list";
    });
    // Удалённый logout / отзыв сессии (AUTH_KEY_DUPLICATED): останавливаем сервис — с
    // restart-политикой контейнер поднимется и честно попросит новый логин, вместо вечных 409.
    // queueInLoop до run() просто отложит quit до старта loop'а.
    auth.setOnUnexpectedTermination([] {
        LOG_ERROR << "session terminated remotely (logout/revoked) — shutting down";
        drogon::app().getLoop()->queueInLoop([] { drogon::app().quit(); });
    });

    bridge.start();

    // Автоподъём сессии ДО приёма HTTP (§7.1): глушим лог, шлём setTdlibParameters.
    tgw::auth::StartupBootstrapper::run(bridge, client_id, config, auth);

    const std::string upload_dir = config.files_directory + "/uploads";
    std::error_code mkdir_ec;
    std::filesystem::create_directories(upload_dir, mkdir_ec);
    drogon::app().setClientMaxBodySize(config.max_upload_bytes);
    // Тела крупнее порога Drogon спулит в temp-файл (mmap) — 64MiB-аплоад не живёт в RAM.
    drogon::app().setClientMaxMemoryBodySize(config.max_memory_body_bytes);

    tgw::http::registerRoutes(bridge, client_id, auth, config.database_directory);
    tgw::http::registerMessageRoutes(bridge, client_id, upload_dir);
    tgw::http::registerDirectoryRoutes(bridge, client_id);
    tgw::ws::WsSubscriberRegistry::instance().setMaxPendingBytes(config.ws_max_pending_bytes);
    tgw::ws::UpdatesWs::setSessionId(config.session_id);
    tgw::http::registerLoginUi();  // GET /ui — страница входа (форма/QR)
    tgw::http::registerMetricsRoutes(bridge, auth);  // GET /metrics (Prometheus)
    tgw::http::startUploadCleanup(upload_dir, std::chrono::hours(1));

    // ВАЖНО: НЕ поллим продюсер с главного loop. produce()+poll(0) уже идут на потоке-приёмнике
    // TDLib (единственный, кто трогает продюсер); отдельный runEvery-поллер на loop-потоке давал
    // конкурентный доступ к RdKafka::Producer -> heap corruption на amd64 (arm64 терпел). Продюсер
    // теперь single-owner: приёмник во время работы, shutdown-flush уже ПОСЛЕ join приёмника.

    // Периодический бэкап сессии в S3 (no-op если S3 не сконфигурирован). in_flight нужен, чтобы
    // на shutdown дождаться незавершённого фонового PUT перед финальным push.
    auto s3_sync_in_flight = tgw::auth::startS3Sync(config.s3, config.database_directory,
                                                    config.s3_sync_interval_seconds);

    LOG_INFO << "telegram-rest-gateway listening on " << config.listen_address << ":"
             << config.listen_port;
    drogon::app().addListener(config.listen_address, config.listen_port).run();

    // Graceful shutdown (§7.4a, §10.4): даём TDLib закрыть БД до выхода — иначе при docker stop
    // возможна порча БД. run() уже вернулся (SIGTERM/SIGINT), но поток-приёмник ещё жив и
    // применит authorizationStateClosed, разбудив waitForClosed.
    LOG_INFO << "shutting down: closing TDLib client";
    auth.expectShutdown();  // дальше терминальные состояния ожидаемы
    bridge.sendOneWay(client_id, td_api::make_object<td_api::close>());
    if (!auth.waitForClosed(std::chrono::seconds(10))) {
        LOG_WARN << "TDLib did not reach Closed within timeout";
    }
    bridge.stop();

    // Дожидаемся доставки хвоста событий в Kafka (не блокирует, если очередь пуста/выключено).
    if (kafka) {
        kafka->flush(std::chrono::seconds(10));
    }

    // Финальный push сессии в S3: binlog закрыт TDLib и консистентен (no-op если S3 выключен).
    if (config.s3.enabled()) {
        // Дожидаемся незавершённого фонового sync-PUT, иначе он мог бы затереть чистый снапшот
        // старым. Loop уже остановлен, новые sync-задачи не спаунятся.
        for (int i = 0; i < 200 && s3_sync_in_flight->load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // максимум ~10 с
        }
        if (tgw::auth::pushToS3(config.s3, config.database_directory)) {
            LOG_INFO << "session pushed to S3 on shutdown";
        } else {
            LOG_ERROR << "failed to push session to S3 on shutdown";
        }
    }
    return EXIT_SUCCESS;
}
