#pragma once

#include "webhook/context_builder.hpp"
#include "webhook/webhook_registry.hpp"

#include <trantor/net/EventLoopThread.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace tgw::webhook {

// Подпись тела вебхука — чистая функция (тестируется отдельно от сети). Возвращает
// "sha256=<hex>", где hex — строчный HMAC-SHA256(secret, body). hmacSha256 отдаёт сырые
// байты, хексим их локально (готового hex-конвертера произвольных байт в util нет).
std::string signBody(const std::string& secret, const std::string& body);

// Сериализация события в компактный JSON (как compact() в update_router: без отступов).
// Именно эта строка уходит в тело POST'а и подписывается — вынесена наружу ради тестов.
std::string serializeEvent(const WebhookEvent& ev);

// Воркер-пул доставки вебхуков. АРХИТЕКТУРА (аналог инварианта KafkaSink/S3Client):
// вся работа с drogon::HttpClient (создание клиента, sendRequest, колбэки) идёт РОВНО на
// одном выделенном loop-потоке (loop_thread_). dispatch() вызывается из потока-приёмника
// моста и НИКОГДА не трогает HttpClient — только сериализует событие и кладёт строку в
// очередь под mutex+cv (drop при переполнении → webhook_dropped_total), будя воркер.
// Воркер достаёт батч, снимает active-снапшот реестра (лочит реестр ВНЕ loop-потока) и
// маршалит каждую доставку на loop-поток через queueInLoop.
//
// ЛАЙФТАЙМ: функтор доставки, поставленный на loop, СЕЛФ-КОНТЕЙНЕД — не захватывает this
// (копирует hook/event_id/body/таймаут/loop по значению) и создаёт свой HttpClient. Поэтому
// straggler-функтор, который trantor может докрутить в ~EventLoopThread уже после сноса
// членов диспетчера, не обращается к освобождённой памяти (класс UAF, на котором горел
// S3/Kafka). Плата — нет keep-alive-кэша клиентов: на fan-out по разным URL при объёме
// mention/reply-событий переиспользование соединений вторично против безопасности лайфтайма.
class WebhookDispatcher {
   public:
    WebhookDispatcher(WebhookRegistry& reg, int timeout_ms, std::size_t queue_max, bool ssrf_guard);
    ~WebhookDispatcher();  // джойн воркера (снос loop-потока — в ~EventLoopThread)

    WebhookDispatcher(const WebhookDispatcher&) = delete;
    WebhookDispatcher& operator=(const WebhookDispatcher&) = delete;

    void start();                           // поднять loop + воркер
    void dispatch(const WebhookEvent& ev);  // неблокирующе: сериализовать + enqueue (drop)
    void stop();                            // остановить воркер

   private:
    void workerLoop();

    WebhookRegistry& reg_;
    const double timeout_s_;  // таймаут sendRequest в секундах (drogon-API — double)
    const std::size_t queue_max_;
    const bool ssrf_guard_;

    // loop-поток, на котором живёт весь HttpClient-код. Создаётся в конструкторе (trantor
    // стартует поток сразу), реально начинает крутиться после run() в start().
    trantor::EventLoopThread loop_thread_{"webhook-dispatch"};

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
