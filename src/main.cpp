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
#include "http/webhook_routes.hpp"
#include "util/s3_client.hpp"
#include "webhook/context_builder.hpp"
#include "webhook/webhook_dispatcher.hpp"
#include "webhook/webhook_registry.hpp"
#include "ws/owner_mention_detector.hpp"
#include "ws/update_router.hpp"
#include "ws/updates_ws.hpp"
#include "ws/ws_registry.hpp"

#include <drogon/drogon.h>
#include <drogon/HttpAppFramework.h>
#include <td/telegram/td_api.h>
#include <trantor/net/EventLoopThread.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace td_api = td::td_api;

namespace {

constexpr std::uint16_t kDefaultPort = 8080;

// Подкоманда для distroless HEALTHCHECK (нет shell/curl): локальный GET /v1/health.
// Секретов не требует (Config не грузим), чтобы HEALTHCHECK работал всегда.
// [[noreturn]]: выходим через _Exit, чтобы не гонять static destruction при намеренно
// утёкшем EventLoopThread (см. ниже).
[[noreturn]] void runHealthcheck() {
    std::uint16_t port = kDefaultPort;
    if (const char* env = std::getenv("TGW_LISTEN_PORT")) {
        port = static_cast<std::uint16_t>(std::atoi(env));
    }
    // Здесь app().run() не вызывается, главного event-loop нет — синхронный sendRequest завис бы
    // навсегда (и HEALTHCHECK стабильно валился по таймауту). Крутим запрос на собственном
    // EventLoopThread. Поток намеренно НЕ останавливаем и не разрушаем (new без delete):
    // quit()+wait() под ещё живым запросом (колбэк не успел за 2500 мс — медленный контейнер)
    // портит кучу — тот же teardown-паттерн, из-за которого util/s3_client перевели на
    // долгоживущий loop. Завершаемся через _Exit: ОС заберёт потоки, static dtor'ы не трогаем
    // (иначе ~mutex / Drogon statics могут пересечься с ещё живым s3/healthcheck loop-потоком;
    // LSan в prod HEALTHCHECK не гоняем).
    auto* loop_thread = new trantor::EventLoopThread("healthcheck");
    loop_thread->run();
    auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(port),
                                                    loop_thread->getLoop());
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/v1/health");

    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    // client захвачен в колбэк как страховка на случай выхода по wait_for раньше ответа:
    // HttpClientImpl и сам держит shared_from_this на время запроса, но полагаться только
    // на эту внутреннюю деталь drogon не хотим.
    client->sendRequest(
        req,
        [promise, client](drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
            promise->set_value(result == drogon::ReqResult::Ok && resp != nullptr &&
                               resp->statusCode() == drogon::k200OK);
        },
        2.0);

    bool ok = false;
    if (future.wait_for(std::chrono::milliseconds(2500)) == std::future_status::ready) {
        ok = future.get();
    }
    // _Exit не флашит stdio: в distroless stdout — не-tty (полная буферизация), без явного
    // flush диагностика упавшей пробы (LOG_ERROR trantor/drogon) терялась бы.
    std::fflush(nullptr);
    std::_Exit(ok ? EXIT_SUCCESS : EXIT_FAILURE);
}

// Единая точка выхода из main() после первого возможного обращения к S3 (все return —
// через неё). Если s3 loop-поток нельзя гарантированно погасить (активный send() или ранее
// списанный зависший поток), обычный return прогнал бы static destruction по живому потоку —
// тот самый класс краша, который здесь чиним. Тогда выходим через _Exit: stdio флашим явно,
// пропуск деструкторов локалов main безопасен (TDLib закрыт, Kafka сброшена к этому моменту).
int exitCodeAfterS3(int code) {
    if (tgw::util::S3Client::shutdownIdleLoop()) {
        return code;
    }
    LOG_WARN << "s3 loop thread not cleanly stopped — exiting via _Exit "
                "(skipping static destruction)";
    std::fflush(nullptr);
    std::_Exit(code);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--healthcheck") {
        runHealthcheck();  // noreturn (_Exit)
    }

    // §2.1 graceful shutdown: сами обрабатываем SIGTERM/SIGINT. Дефолтный хендлер Drogon зовёт
    // quit() сразу, а quit() джойнит IO-петли ДО возврата run() — тогда resume in-flight корутин,
    // маршаленный в петлю через queueInLoop, теряется, и HTTP-хендлер повисает молча. Блокируем
    // сигналы во ВСЕХ потоках (маску наследуют все создаваемые ниже потоки: s3-loop, приёмник
    // TDLib, kafka-воркер, IO-потоки Drogon) и ждём их в одном выделенном потоке через sigwait() —
    // обработчик не ставим вовсе, поэтому async-signal-safety не при чём. Ставить маску нужно до
    // старта любых потоков, поэтому делаем это в самом начале main.
    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGTERM);
    sigaddset(&shutdown_signals, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr) != 0) {
        std::cerr << "failed to block shutdown signals\n";
        return EXIT_FAILURE;
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
    // С этого места любой выход из main — через exitCodeAfterS3 (гасит idle s3-loop; при
    // зависшем/утёкшем потоке уходит в _Exit вместо static destruction).
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
            return exitCodeAfterS3(EXIT_FAILURE);
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
            return exitCodeAfterS3(EXIT_FAILURE);
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

    // --- Вебхуки mention/reply (§вебхуки): реестр + диспетчер + owner_id ---
    // Держатели живут во всём scope main: hook (в router) и REST-роуты (registerWebhookRoutes)
    // ссылаются на реестр/диспетчер, а shutdown-хвост зовёт dispatcher->stop(). Объявлены ПОСЛЕ
    // bridge/router, поэтому при выходе из scope разрушаются РАНЬШЕ них — но к тому моменту и
    // bridge.stop(), и dispatcher->stop() уже вызваны явно, так что хук более не дёргается.
    // owner_id — атомик в shared_ptr: пишется корутиной getMe (резолв на loop'е Drogon), читается
    // из хука (поток-приёмник). shared_ptr развязывает время жизни от локалов main (self-contained
    // захват в обе лямбды). Создаётся всегда (одна дешёвая аллокация на старте), но резолвится и
    // читается только при webhooks_enabled — при выключенной фиче хук не ставится (ноль оверхеда).
    auto owner_id = std::make_shared<std::atomic<std::int64_t>>(0);
    std::unique_ptr<tgw::webhook::IWebhookStore> webhook_store;
    std::unique_ptr<tgw::webhook::WebhookRegistry> webhook_registry;
    std::unique_ptr<tgw::webhook::WebhookDispatcher> webhook_dispatcher;
    const bool webhooks_enabled = config.webhooks_enabled;
    if (webhooks_enabled) {
        // S3-стор реестра — соседний с td.binlog объект: та же bucket/creds/endpoint, но ключ
        // <dir(session_key)>/webhooks.json (по аналогии с auth/s3_session, но другой suffix).
        tgw::util::S3Config webhook_s3 = config.s3;
        const std::string& session_key = config.s3.key;
        const auto slash = session_key.find_last_of('/');
        webhook_s3.key =
            (slash == std::string::npos ? std::string{} : session_key.substr(0, slash + 1)) +
            "webhooks.json";
        const std::string webhook_key = webhook_s3.key;

        webhook_store = tgw::webhook::makeS3WebhookStore(std::move(webhook_s3));
        webhook_registry = std::make_unique<tgw::webhook::WebhookRegistry>(*webhook_store);
        webhook_registry->loadFromStore();
        webhook_dispatcher = std::make_unique<tgw::webhook::WebhookDispatcher>(
            *webhook_registry, config.webhook_timeout_ms, config.webhook_queue_max,
            config.webhook_ssrf_guard);
        webhook_dispatcher->start();

        // Хук вызывается синхронно из потока-приёмника с ЖИВОЙ ссылкой на message (принадлежит
        // апдейту, до конца onUpdate). Тип чата определяем СИНХРОННО из полей message, БЕЗ getChat:
        // getChat потребовал бы co_await ДО detect, а message — move-only и не переживает suspend
        // (владения из const& у нас нет), поэтому дешёвый lookup из самого сообщения
        // (санкционирован брифом: "private/broadcast определить по chat_id знаку/типу"): личка ⟺
        // chat_id_ > 0 (private-чат = user_id), broadcast-канал ⟺ is_channel_post_. detect — чистый
        // и быстрый. При срабатывании запускаем EAGER-корутину (drogon::AsyncTask) ПРЯМО тут: её
        // префикс — buildEvent(...) — снимает всё нужное из message ДО первого co_await
        // (getMessage), пока message ещё жив; на первом suspend await_suspend в потоке-приёмнике не
        // находит loop и берёт главный loop Drogon — resume/dispatch идут уже на нём,
        // поток-приёмник не блокируется сетевым I/O. После первого suspend ни AsyncTask, ни
        // buildEvent к message не обращаются (контракт buildEvent), поэтому висящая ссылка
        // безопасна.
        router.setWebhookHook([&bridge, client_id, owner_id, dispatcher = webhook_dispatcher.get(),
                               session_id = config.session_id](const td::td_api::message& msg) {
            const bool chat_is_private = msg.chat_id_ > 0;
            const bool chat_is_broadcast = msg.is_channel_post_;
            const std::int64_t oid = owner_id->load(std::memory_order_relaxed);
            const tgw::ws::DetectResult det =
                tgw::ws::detect(msg, oid, chat_is_private, chat_is_broadcast);
            if (!det.triggered && !det.reply_pending) {
                return;  // не триггер — ничего не планируем (горячий путь дешёвый)
            }
            const std::int32_t received_at = msg.date_;
            [](tgw::bridge::TdBridge& td, std::int32_t cid, const td::td_api::message& m,
               tgw::ws::DetectResult d, std::int64_t o, std::string sid, std::int32_t rcv,
               tgw::webhook::WebhookDispatcher* disp) -> drogon::AsyncTask {
                auto ev = co_await tgw::webhook::buildEvent(td, cid, m, d, o, std::move(sid), rcv);
                if (ev) {
                    disp->dispatch(*ev);
                }
                co_return;
            }(bridge, client_id, msg, det, oid, session_id, received_at, dispatcher);
        });
        LOG_INFO << "webhooks enabled (registry store key " << webhook_key << ")";
    }

    // Колбэки регистрируем ДО start(): восстановленная из S3/TGW_SESSION сессия доходит до
    // Ready ещё в StartupBootstrapper — регистрация после него теряет событие.
    // Warmup чатов (§бэклог ChatCache): TDLib наполняет главный список лениво — без прогрева
    // первый GET /v1/chats после старта может отдать частичный список. Fire-and-forget.
    const bool keep_online = config.keep_online;
    auth.setOnReady([&bridge, client_id, keep_online, webhooks_enabled, owner_id] {
        auto load = td_api::make_object<td_api::loadChats>();
        load->chat_list_ = td_api::make_object<td_api::chatListMain>();
        load->limit_ = 100;
        bridge.sendOneWay(client_id, std::move(load));
        LOG_INFO << "authorized: warming up main chat list";
        // keep-online: сразу после авторизации выставляем online (TDLib по умолчанию offline).
        // Тот же поток-приёмник, что и loadChats — потокобезопасно; порядок неважен.
        if (keep_online) {
            bridge.sendOneWay(client_id,
                              td_api::make_object<td_api::setOption>(
                                  "online", td_api::make_object<td_api::optionValueBoolean>(true)));
            LOG_INFO << "keep-online: setting online=true";
        }
        // Вебхуки: резолвим owner_id (id владельца аккаунта) через getMe. Detached-корутина —
        // co_await ответа, на успехе кладём id в атомик (читается детектором из потока-приёмника).
        // Запускается из потока-приёмника; первый suspend уводит resume на loop Drogon (там и
        // происходит store). Повтор при каждом Ready безвреден: id стабилен, запись идемпотентна.
        if (webhooks_enabled) {
            [](tgw::bridge::TdBridge& td, std::int32_t cid,
               std::shared_ptr<std::atomic<std::int64_t>> oid) -> drogon::AsyncTask {
                auto obj = co_await td.invoke(cid, td_api::make_object<td_api::getMe>());
                if (obj != nullptr && obj->get_id() == td_api::user::ID) {
                    oid->store(static_cast<const td_api::user&>(*obj).id_,
                               std::memory_order_relaxed);
                    LOG_INFO << "webhooks: resolved owner_id";
                }
                co_return;
            }(bridge, client_id, owner_id);
        }
    });
    // keep-online: переустанавливаем online при КАЖДОМ восстановлении соединения
    // (connectionStateReady), чтобы статус пережил реконнекты. Колбэк регистрируем ДО start()
    // (пишется до старта потока-приёмника, вызывается из него после) — гонки на std::function нет.
    if (keep_online) {
        router.setOnConnectionReady([&bridge, client_id] {
            bridge.sendOneWay(client_id,
                              td_api::make_object<td_api::setOption>(
                                  "online", td_api::make_object<td_api::optionValueBoolean>(true)));
        });
        // keep-online: периодическая страховка поверх событийных хуков выше. TDLib НЕ поддерживает
        // online-статус сама — без периодического подтверждения от клиента он деградирует по
        // истечении внутреннего таймаута TDLib. setOnReady/setOnConnectionReady покрывают старт и
        // реконнект, но соединение может простоять стабильным (без единого connectionStateReady)
        // часами — именно этот случай прод и показал: за 5+ часов работы без рестартов
        // connectionStateReady сработал ровно один раз. runEvery закрывает этот пробел, повторяя
        // setOption на фиксированном интервале независимо от событий соединения. Действие лёгкое
        // (sendOneWay — постановка в очередь моста, не блокирующий сетевой вызов), поэтому, в
        // отличие от startS3Sync (src/auth/s3_session.cpp), отдельный поток не нужен — колбэк
        // выполняется прямо на главном event-loop Drogon. Регистрируется ДО bridge.start(), но
        // исполняется только после старта loop'а (drogon::app().run()) — как и весь keep-online
        // блок и upload_cleanup.cpp, это штатно.
        drogon::app().getLoop()->runEvery(
            static_cast<double>(config.keep_online_interval_seconds), [&bridge, client_id] {
                bridge.sendOneWay(
                    client_id,
                    td_api::make_object<td_api::setOption>(
                        "online", td_api::make_object<td_api::optionValueBoolean>(true)));
            });
    }
    // Удалённый logout / отзыв сессии (AUTH_KEY_DUPLICATED): останавливаем сервис — с
    // restart-политикой контейнер поднимется и честно попросит новый логин, вместо вечных 409.
    // queueInLoop до run() просто отложит quit до старта loop'а.
    auth.setOnUnexpectedTermination([] {
        LOG_ERROR << "session terminated remotely (logout/revoked) — shutting down";
        drogon::app().getLoop()->queueInLoop([] { drogon::app().quit(); });
    });

    bridge.start();

    // Выделенный поток ожидания сигналов завершения (маску мы выставили в начале main).
    // Порядок §2.1: сперва дренируем мост (резолвим все in-flight 503/UPSTREAM_SHUTDOWN, пока
    // IO-петли Drogon ещё живы — resume реально проснётся и отдаст ответ), затем quit() (маршалим
    // на главный loop, как в setOnUnexpectedTermination). Набор сигналов копируем в поток по
    // значению, чтобы sigwait() не смотрел на локал main после его разрушения. Поток detached
    // намеренно (как s3-sync/healthcheck-потоки): при удалённом logout сигнал не придёт вовсе —
    // поток так и стоит в sigwait() до выхода процесса и ничего не трогает (bridge к тому моменту
    // отработал штатный shutdown-хвост ниже, чей bridge.stop() дренирует уже как backstop).
    std::thread([&bridge, shutdown_signals]() {
        int sig = 0;
        if (sigwait(&shutdown_signals, &sig) != 0) {
            return;  // не должно случаться при валидном наборе
        }
        LOG_INFO << "received signal " << sig << ": draining bridge before quit";
        bridge.drainPending();
        drogon::app().getLoop()->queueInLoop([] { drogon::app().quit(); });
    }).detach();

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
    if (webhook_registry) {
        tgw::http::registerWebhookRoutes(*webhook_registry);  // POST/GET /v1/webhooks, DELETE /{id}
    }
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
    // Отключаем встроенный SIGTERM-хендлер Drogon: завершение ведёт наш sigwait-поток (§2.1),
    // который дренирует мост перед quit(). (Сигналы всё равно заблокированы во всех потоках —
    // это ещё и явно фиксирует намерение.)
    drogon::app().disableSigtermHandling();
    drogon::app().addListener(config.listen_address, config.listen_port).run();

    // Graceful shutdown (§7.4a, §10.4): даём TDLib закрыть БД до выхода — иначе при docker stop
    // возможна порча БД. run() уже вернулся (наш sigwait-поток продренировал мост и вызвал
    // quit(), либо удалённый logout вызвал quit() напрямую), но поток-приёмник ещё жив и
    // применит authorizationStateClosed, разбудив waitForClosed. bridge.stop() ниже дренирует
    // «опоздавшие» in-flight как backstop (петли уже мертвы — resolve() резюмирует синхронно).
    LOG_INFO << "shutting down: closing TDLib client";
    auth.expectShutdown();  // дальше терминальные состояния ожидаемы
    bridge.sendOneWay(client_id, td_api::make_object<td_api::close>());
    if (!auth.waitForClosed(std::chrono::seconds(10))) {
        LOG_WARN << "TDLib did not reach Closed within timeout";
    }
    bridge.stop();

    // Диспетчер вебхуков гасим СТРОГО ПОСЛЕ bridge.stop(): пока мост жив, поток-приёмник может
    // дёргать хук, а drainPending()/stop() резюмирует in-flight buildEvent-корутины (при мёртвом
    // loop'е — синхронно на потоке-приёмнике), и те вызывают dispatch(). Только после join'а
    // приёмника новых dispatch() быть не может — тогда безопасно дренировать in-flight HTTP и
    // снести loop диспетчера. stop() идемпотентен и дублируется деструктором как backstop.
    if (webhook_dispatcher) {
        webhook_dispatcher->stop();
    }

    // Дожидаемся доставки хвоста событий в Kafka (не блокирует, если очередь пуста/выключено).
    if (kafka) {
        kafka->flush(std::chrono::seconds(10));
    }

    // Финальный push сессии в S3: binlog закрыт TDLib и консистентен (no-op если S3 выключен).
    if (config.s3.enabled()) {
        // Дожидаемся незавершённого фонового sync-PUT. Loop Drogon уже остановлен — новых
        // runEvery не будет, но detached-поток мог стартовать до quit. Бюджет = hang-timeout
        // send() + запас: до send() detached-поток ещё спаунится, читает и хэширует binlog,
        // так что сам kSendHangTimeout верхней границей всего цикла не является.
        constexpr auto kBgSyncWait =
            tgw::util::S3Client::kSendHangTimeout + std::chrono::seconds(10);
        constexpr auto kPoll = std::chrono::milliseconds(50);
        std::chrono::milliseconds waited{0};
        while (s3_sync_in_flight->load(std::memory_order_acquire) && waited < kBgSyncWait) {
            std::this_thread::sleep_for(kPoll);
            waited += kPoll;
        }
        if (s3_sync_in_flight->load(std::memory_order_acquire)) {
            LOG_ERROR << "background s3 sync still in flight after "
                      << std::chrono::duration_cast<std::chrono::seconds>(kBgSyncWait).count()
                      << "s (likely hung s3 loop) — pushing final snapshot anyway";
        }
        // Финальный push делаем ВСЕГДА — пропуск оставлял бы в S3 устаревший снапшот при
        // следующем cold start. Порядок относительно фонового PUT безопасен: на живом loop'е
        // оба идут через один кэшированный клиент (одно соединение — порядок гарантирован);
        // обогнать может только брошенный PUT списанного зависшего контекста на другом
        // соединении — от этого защищает лишь versioning бакета (send() пишет об этом ERROR
        // при retire). Весь бюджет shutdown согласован с terminationGracePeriodSeconds
        // в deploy/helm (см. values.yaml).
        if (tgw::auth::pushToS3(config.s3, config.database_directory)) {
            LOG_INFO << "session pushed to S3 on shutdown";
        } else {
            LOG_ERROR << "failed to push session to S3 on shutdown";
        }
    }
    // Гасит idle s3-loop; при зависшем/утёкшем s3-потоке или незавершённом send() уходит в
    // _Exit — обычный return прогнал бы static destruction по живому потоку.
    return exitCodeAfterS3(EXIT_SUCCESS);
}
