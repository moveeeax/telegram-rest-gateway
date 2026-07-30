#include "webhook/webhook_dispatcher.hpp"

#include "util/aws_sigv4.hpp"
#include "util/metrics.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <json/value.h>
#include <json/writer.h>
#include <string>
#include <utility>

namespace tgw::webhook {
namespace {

// Строчный hex произвольных байт. hmacSha256 отдаёт сырые байты, а нам нужен
// "sha256=<hex>"; готового hex-конвертера для байт в util нет (toHex в aws_sigv4.cpp
// приватный), поэтому маленький локальный хелпер.
std::string toHexLower(const std::string& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char c : bytes) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

// Разбор URL вебхука на base (scheme://host[:port]) + path-with-query + host (для SSRF).
struct ParsedUrl {
    std::string base;  // http(s)://host[:port]
    std::string path;  // /path?query — как есть (не перекодируем, см. setPathEncode(false))
    std::string host;  // host без порта/скобок — для SSRF-проверки
    bool valid = false;
};

ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl p;
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return p;  // без схемы не работаем (drogon HttpClient требует base со схемой)
    }
    const std::string scheme = url.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https") {
        return p;
    }
    const std::string rest = url.substr(scheme_end + 3);
    const auto slash = rest.find('/');
    const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    if (authority.empty()) {
        return p;
    }
    p.base = scheme + "://" + authority;
    p.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    // Выделяем host из authority (может быть [ipv6]:port или host:port).
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        p.host =
            (close == std::string::npos) ? authority.substr(1) : authority.substr(1, close - 1);
    } else {
        const auto colon = authority.find(':');
        p.host = (colon == std::string::npos) ? authority : authority.substr(0, colon);
    }
    p.valid = true;
    return p;
}

// Грубая, но безопасная-по-умолчанию проверка приватного/loopback-хоста для SSRF-guard.
// Работает по строке хоста: literal-IP разбираем, доменные имена (кроме localhost) считаем
// внешними (DNS-rebinding вне охвата — резолвинг у drogon, не у нас). Смысл — не дать
// админскому URL целиться в 127.0.0.1/10.x/метадату-сервис без явного отключения guard'а.
bool isPrivateHost(const std::string& host) {
    if (host.empty() || host == "localhost") {
        return true;
    }
    // IPv6: loopback ::1, unspecified ::, unique-local fc00::/7, link-local fe80::/10.
    if (host.find(':') != std::string::npos) {
        if (host == "::1" || host == "::") {
            return true;
        }
        if (host.size() >= 2) {
            const char a = host[0];
            const char b = host[1];
            if ((a == 'f' || a == 'F') && (b == 'c' || b == 'C' || b == 'd' || b == 'D')) {
                return true;  // fc00::/7 unique-local
            }
            if ((a == 'f' || a == 'F') && (b == 'e' || b == 'E')) {
                return true;  // fe80::/10 link-local (грубо: любой fe..)
            }
        }
        return false;
    }
    // IPv4-октеты. Не число в первом октете → доменное имя, считаем внешним.
    unsigned int o0 = 0, o1 = 0, o2 = 0, o3 = 0;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u", &o0, &o1, &o2, &o3) != 4) {
        return false;
    }
    if (o0 == 127u || o0 == 10u || o0 == 0u) {
        return true;  // loopback / RFC1918 10/8 / 0.0.0.0
    }
    if (o0 == 192u && o1 == 168u) {
        return true;  // 192.168/16
    }
    if (o0 == 172u && o1 >= 16u && o1 <= 31u) {
        return true;  // 172.16/12
    }
    if (o0 == 169u && o1 == 254u) {
        return true;  // link-local + облачная metadata (169.254.169.254)
    }
    return false;
}

// Одна доставка. Выполняется ТОЛЬКО на loop-потоке (вызывается из lambda в queueInLoop):
// единственное место, где создаётся и трогается HttpClient. Селф-контейнед — НЕ зависит от
// диспетчера (все аргументы по значению), поэтому безопасна даже как straggler в teardown'е.
// Fire-and-forget: колбэк лишь инкрементит глобальные счётчики.
void deliverOnLoop(trantor::EventLoop* loop, const Webhook& hook, const std::string& event_id,
                   const std::string& body, double timeout_s, bool ssrf_guard) {
    auto& counters = tgw::metrics::Counters::instance();

    const ParsedUrl url = parseUrl(hook.url);
    if (!url.valid) {
        counters.webhook_failed_total.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR << "webhook delivery skipped: unparseable url for hook " << hook.id;
        return;
    }
    if (ssrf_guard && isPrivateHost(url.host)) {
        counters.webhook_failed_total.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR << "webhook delivery blocked by ssrf-guard: private host " << url.host
                  << " (hook " << hook.id << ")";
        return;
    }

    // Клиент на этот запрос, привязанный к текущему loop'у (без keep-alive-кэша, см. hpp).
    auto client = drogon::HttpClient::newHttpClient(url.base, loop);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPathEncode(false);  // path берём из URL как есть, drogon его не перекодирует
    req->setPath(url.path);
    req->setContentTypeString("application/json");
    req->addHeader("X-TGW-Signature", signBody(hook.secret, body));
    req->addHeader("X-TGW-Event-Id", event_id);
    req->setBody(body);

    client->sendRequest(
        req,
        // Держим client в колбэке живым до завершения запроса; this НЕ захватываем — колбэк
        // самодостаточен (глобальные счётчики + копии строк для лога).
        [client, hook_id = hook.id, event_id](drogon::ReqResult result,
                                              const drogon::HttpResponsePtr& resp) {
            auto& c = tgw::metrics::Counters::instance();
            if (result != drogon::ReqResult::Ok || resp == nullptr) {
                c.webhook_failed_total.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN << "webhook transport error (hook " << hook_id << " event " << event_id
                         << "): " << static_cast<int>(result);
                return;
            }
            const int status = resp->statusCode();
            if (status >= 200 && status < 300) {
                c.webhook_delivered_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                c.webhook_failed_total.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN << "webhook non-2xx (hook " << hook_id << " event " << event_id
                         << "): status " << status;
            }
        },
        timeout_s);
}

}  // namespace

std::string signBody(const std::string& secret, const std::string& body) {
    // hmacSha256 → сырые байты; хексим и предваряем алгоритмом (совместимо с GitHub-стилем).
    return "sha256=" + toHexLower(tgw::util::hmacSha256(secret, body));
}

std::string serializeEvent(const WebhookEvent& ev) {
    Json::Value root(Json::objectValue);
    root["event_id"] = ev.event_id;
    root["session_id"] = ev.session_id;
    root["owner_id"] = ev.owner_id;
    root["trigger_reason"] = ev.trigger_reason;
    root["received_at"] = ev.received_at;
    root["message"] = ev.message;
    root["reply_chain"] = ev.reply_chain;
    root["chain_truncated"] = ev.chain_truncated;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";  // компактно, как compact() в update_router
    return Json::writeString(builder, root);
}

WebhookDispatcher::WebhookDispatcher(WebhookRegistry& reg, int timeout_ms, std::size_t queue_max,
                                     bool ssrf_guard)
    : reg_(reg),
      timeout_s_(static_cast<double>(timeout_ms) / 1000.0),
      queue_max_(queue_max),
      ssrf_guard_(ssrf_guard) {}

WebhookDispatcher::~WebhookDispatcher() {
    stop();
}

void WebhookDispatcher::start() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            return;
        }
        started_ = true;
        stop_ = false;
    }
    loop_thread_.run();  // поднимаем loop-поток (на нём живёт HttpClient)
    worker_ = std::thread([this] { workerLoop(); });
    LOG_INFO << "webhook dispatcher started (timeout=" << timeout_s_
             << "s queue_max=" << queue_max_ << " ssrf_guard=" << (ssrf_guard_ ? 1 : 0) << ")";
}

void WebhookDispatcher::dispatch(const WebhookEvent& ev) {
    // Сериализуем ВНЕ лока (может быть не быстро) — под локом только enqueue/drop.
    std::string body = serializeEvent(ev);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stop_) {
            return;  // не принимаем до start()/после stop()
        }
        if (queue_.size() >= queue_max_) {
            // Воркер/приёмники не успевают — дропаем (сервис первичен), с прореженным логом.
            static std::atomic<std::uint64_t> log_gate{0};
            const auto dropped =
                tgw::metrics::Counters::instance().webhook_dropped_total.fetch_add(
                    1, std::memory_order_relaxed) +
                1;
            if (log_gate.fetch_add(1, std::memory_order_relaxed) % 1000 == 0) {
                LOG_ERROR << "webhook queue full, dropped (total " << dropped << ")";
            }
            return;
        }
        queue_.push_back(Payload{ev.event_id, std::move(body)});
    }
    cv_.notify_one();
}

// Воркер (отдельный поток): ждёт очередь, снимает active-снапшот реестра (лочит mutex
// реестра — намеренно ВНЕ loop-потока, чтобы не держать loop на этой блокировке) и
// маршалит доставку каждого (payload × hook) на loop-поток. Сам HttpClient НЕ трогает.
void WebhookDispatcher::workerLoop() {
    std::deque<Payload> batch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            batch.swap(queue_);
            if (batch.empty() && stop_) {
                return;
            }
        }
        const auto hooks = reg_.activeSnapshot();  // копия под локом реестра
        auto* loop = loop_thread_.getLoop();
        for (const auto& payload : batch) {
            for (const auto& hook : hooks) {
                // Селф-контейнед функтор: копии + сырой loop-указатель, без this (см. hpp).
                loop->queueInLoop([loop, hook, event_id = payload.event_id, body = payload.body,
                                   timeout_s = timeout_s_, ssrf_guard = ssrf_guard_] {
                    deliverOnLoop(loop, hook, event_id, body, timeout_s, ssrf_guard);
                });
            }
        }
        batch.clear();
    }
}

void WebhookDispatcher::stop() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return;  // идемпотентно (в т.ч. повторный вызов из ~WebhookDispatcher)
        }
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    // Воркер присоединён → новых доставок на loop не ставится. Уже поставленные функторы и
    // in-flight-колбэки самодостаточны (не держат this) — их доработает/сбросит ~EventLoopThread
    // при сносе loop_thread_. UAF на членах диспетчера невозможен by design.
    LOG_INFO << "webhook dispatcher stopped";
}

}  // namespace tgw::webhook
