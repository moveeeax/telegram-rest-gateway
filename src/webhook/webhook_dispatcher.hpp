#pragma once

#include "webhook/context_builder.hpp"
#include "webhook/webhook_registry.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace tgw::webhook {

namespace detail {
// Loop-поток с ЯВНОЙ синхронизацией старта (mutex+cv). Определён в .cpp. Заменяет
// trantor::EventLoopThread, чей run()/getLoop() НЕ дают happens-before между созданием
// wakeup-eventfd в конструкторе EventLoop и первым cross-thread queueInLoop (гонка под TSan).
class LoopThread;
}  // namespace detail

// Подпись тела вебхука — чистая функция (тестируется отдельно от сети). Возвращает
// "sha256=<hex>", где hex — строчный HMAC-SHA256(secret, body). hmacSha256 отдаёт сырые
// байты, хексим их локально (готового hex-конвертера произвольных байт в util нет).
std::string signBody(const std::string& secret, const std::string& body);

// Сериализация события в компактный JSON (как compact() в update_router: без отступов).
// Именно эта строка уходит в тело POST'а и подписывается — вынесена наружу ради тестов.
std::string serializeEvent(const WebhookEvent& ev);

// Разбор URL и приватность хоста — вынесены наружу ради тестов (SSRF-guard), как signBody/
// serializeEvent выше. Определения — в .cpp.
struct ParsedUrl {
    std::string base;
    std::string path;
    std::string host;
    bool valid = false;
};
ParsedUrl parseUrl(const std::string& url);
bool isPrivateHost(const std::string& host);

// Воркер-пул доставки вебхуков. АРХИТЕКТУРА (аналог инварианта KafkaSink/S3Client):
// вся работа с drogon::HttpClient (создание клиента, sendRequest, колбэки) идёт РОВНО на
// одном выделенном loop-потоке. dispatch() вызывается из потока-приёмника моста и НИКОГДА
// не трогает HttpClient — только сериализует событие и кладёт строку в очередь под mutex+cv
// (drop при переполнении → webhook_dropped_total), будя воркер. Воркер достаёт батч, снимает
// active-снапшот реестра (лочит реестр ВНЕ loop-потока) и маршалит каждую доставку на
// loop-поток через queueInLoop.
//
// ЛАЙФТАЙМ (два независимых класса опасности, оба закрыты):
//  1) UAF на членах диспетчера. Функтор доставки и sendRequest-колбэк СЕЛФ-КОНТЕЙНЕД — не
//     захватывают this (копируют hook/event_id/body/таймаут/loop и shared-счётчик по значению).
//     Straggler-функтор, докрученный trantor в teardown'е, не трогает освобождённую память.
//  2) Порча кучи ВНУТРИ trantor/drogon при сносе EventLoop под живым HTTP-коннектом — тот же
//     класс бага, что задокументирован в s3_client.cpp (heap corruption на amd64). Fire-and-forget
//     к внешним URL почти всегда оставляет in-flight запросы на момент shutdown. Защита — как в
//     S3Client: атомарный счётчик in-flight, снос loop-потока ТОЛЬКО при счётчике == 0; если за
//     разумный таймаут не обнулился (медленный/зависший получатель) — loop-поток намеренно
//     лизится (release), а НЕ сносится под нагрузкой. Счётчик живёт в shared_ptr (его co-держат
//     колбэки и утёкший loop), поэтому декремент безопасен даже после разрушения диспетчера.
//
// Нет keep-alive-кэша клиентов: клиент создаётся на запрос. На fan-out по разным URL при
// объёме mention/reply-событий переиспользование соединений вторично против простоты лайфтайма.
class WebhookDispatcher {
   public:
    WebhookDispatcher(WebhookRegistry& reg, int timeout_ms, std::size_t queue_max, bool ssrf_guard);
    ~WebhookDispatcher();  // джойн воркера + дренаж in-flight перед сносом loop-потока

    WebhookDispatcher(const WebhookDispatcher&) = delete;
    WebhookDispatcher& operator=(const WebhookDispatcher&) = delete;

    void start();                           // поднять loop + воркер
    void dispatch(const WebhookEvent& ev);  // неблокирующе: сериализовать + enqueue (drop)
    void stop();  // остановить воркер, дренировать in-flight, снести loop

   private:
    void workerLoop();

    WebhookRegistry& reg_;
    const double timeout_s_;  // таймаут sendRequest в секундах (drogon-API — double)
    const std::size_t queue_max_;
    const bool ssrf_guard_;

    // Счётчик in-flight HTTP-запросов (инкремент перед sendRequest на loop-потоке, декремент в
    // колбэке). В shared_ptr: колбэк и утёкший при зависании loop co-держат его, поэтому
    // декремент валиден и после разрушения диспетчера.
    std::shared_ptr<std::atomic<std::size_t>> in_flight_ =
        std::make_shared<std::atomic<std::size_t>>(0);

    // loop-поток, на котором живёт весь HttpClient-код. Собственный detail::LoopThread (а не
    // trantor::EventLoopThread) — ради happens-before на старте (см. forward-decl выше). В
    // unique_ptr, чтобы при недренированном in-flight на shutdown его можно было release() —
    // намеренно утечь, а не снести под живым коннектом (см. класс опасности 2). Создаётся в
    // start().
    std::unique_ptr<detail::LoopThread> loop_thread_;

    // Очередь (event_id, body) от dispatch() к воркеру.
    std::mutex mutex_;
    std::condition_variable cv_;
    struct Payload {
        std::string event_id;
        std::string body;
    };
    std::deque<Payload> queue_;
    bool started_ = false;
    bool stop_ = false;

    std::thread worker_;
};

}  // namespace tgw::webhook
