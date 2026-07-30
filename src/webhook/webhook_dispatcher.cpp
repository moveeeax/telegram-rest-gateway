#include "webhook/webhook_dispatcher.hpp"

#include "util/aws_sigv4.hpp"
#include "util/metrics.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <future>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace tgw::webhook {

namespace detail {

// Обёртка над trantor::EventLoop с ЯВНОЙ синхронизацией старта (mutex+cv) — калька с LoopThread
// в tests/td_bridge_test.cpp. Зачем не trantor::EventLoopThread: его run()/getLoop() в этой версии
// trantor публикуют лишь указатель на петлю, но НЕ устанавливают happens-before между конструктором
// EventLoop (где на потоке петли создаётся wakeup-eventfd) и первым queueInLoop из ЧУЖОГО треда
// (тест-/воркер-/stop-тред). Под TSan это гонка на eventfd (создание fd на потоке петли vs его
// чтение в wakeup() из queueInLoop). Здесь HB задаём сами: поток петли создаёт EventLoop, публикует
// указатель под mutex_, а конструктор блокируется на cv_, пока указатель не появится — unlock/lock
// одного мьютекса даёт release/acquire, так что создание eventfd happens-before возврата из
// конструктора, а значит и любого последующего queueInLoop (в т.ч. с воркер-треда, созданного ПОСЛЕ
// конструктора, — через HB старта потока).
//
// Останов — quit() ИЗНУТРИ петли (через queueInLoop), затем join: quit() не дёргается из чужого
// треда по крутящейся петле. Деструктор зовётся ТОЛЬКО когда петля дренирована (in-flight == 0);
// при недренированном/зависшем loop диспетчер лизит объект (unique_ptr::release), и деструктор
// НЕ выполняется — поток остаётся жить, join под нагрузкой не делается.
class LoopThread {
   public:
    LoopThread() {
        thread_ = std::thread([this] {
            trantor::EventLoop loop;  // eventfd создаётся здесь, на этом потоке
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                loop_ = &loop;
            }
            cv_.notify_one();
            loop.loop();  // крутится до quit(); EventLoop разрушится здесь же, на своём потоке
        });
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return loop_ != nullptr; });
    }

    ~LoopThread() {
        // quit исполняется НА потоке петли → нет гонки quit() с loop() из чужого треда.
        loop_->queueInLoop([loop = loop_] { loop->quit(); });
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;

    // loop_ после конструктора не меняется (запись под мьютексом до cv.wait) — читаем без лока.
    trantor::EventLoop* loop() const { return loop_; }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    trantor::EventLoop* loop_ = nullptr;
    std::thread thread_;
};

}  // namespace detail

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

// RAII-токен in-flight: инкремент в конструкторе, декремент при уничтожении. Копию токена
// (через shared_ptr) захватывает sendRequest-колбэк — декремент срабатывает, когда drogon
// уничтожает колбэк: и при штатном завершении, и если колбэк не был вызван (loop снесён).
// Ровно один инкремент/декремент на запрос, без гонок ветвлений.
class InFlightToken {
   public:
    explicit InFlightToken(std::shared_ptr<std::atomic<std::size_t>> counter)
        : counter_(std::move(counter)) {
        counter_->fetch_add(1, std::memory_order_acq_rel);
    }
    ~InFlightToken() { counter_->fetch_sub(1, std::memory_order_acq_rel); }

    InFlightToken(const InFlightToken&) = delete;
    InFlightToken& operator=(const InFlightToken&) = delete;

   private:
    std::shared_ptr<std::atomic<std::size_t>> counter_;
};

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
// Fire-and-forget: колбэк инкрементит глобальные счётчики. in_flight поднимается РОВНО перед
// sendRequest и опускается при уничтожении колбэка (см. InFlightToken) — это гейт корректного
// сноса loop-потока в stop().
void deliverOnLoop(trantor::EventLoop* loop, const Webhook& hook, const std::string& event_id,
                   const std::string& body, double timeout_s, bool ssrf_guard,
                   const std::shared_ptr<std::atomic<std::size_t>>& in_flight) {
    const ParsedUrl url = parseUrl(hook.url);
    if (!url.valid) {
        // Конфиг-ошибка, не провал доставки — только лог, без webhook_failed_total.
        LOG_ERROR << "webhook delivery skipped: unparseable url for hook " << hook.id;
        return;
    }
    if (ssrf_guard && isPrivateHost(url.host)) {
        // Политический блок, не транспортный провал — только лог, без webhook_failed_total.
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

    // Токен поднят перед отправкой; живёт в колбэке (декремент при его уничтожении).
    auto token = std::make_shared<InFlightToken>(in_flight);

    client->sendRequest(
        req,
        // Держим client и token живыми до уничтожения колбэка; this НЕ захватываем — колбэк
        // самодостаточен (глобальные счётчики + копии строк для лога).
        [client, token, hook_id = hook.id, event_id](drogon::ReqResult result,
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
    // Создаём loop-поток тут (не в конструкторе): unique_ptr позволяет при недренированном
    // shutdown его release() (см. stop()). Конструктор detail::LoopThread блокируется до готовности
    // петли (mutex+cv) — happens-before для всех последующих queueInLoop (см. detail::LoopThread).
    loop_thread_ = std::make_unique<detail::LoopThread>();
    worker_ = std::thread([this] { workerLoop(); });
    LOG_INFO << "webhook dispatcher started (timeout=" << timeout_s_ << "s queue_max=" << queue_max_
             << " ssrf_guard=" << (ssrf_guard_ ? 1 : 0) << ")";
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
            const auto dropped = tgw::metrics::Counters::instance().webhook_dropped_total.fetch_add(
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
        auto* loop = loop_thread_->loop();
        for (const auto& payload : batch) {
            for (const auto& hook : hooks) {
                // Селф-контейнед функтор: копии + сырой loop-указатель + shared-счётчик, без this.
                loop->queueInLoop([loop, hook, event_id = payload.event_id, body = payload.body,
                                   timeout_s = timeout_s_, ssrf_guard = ssrf_guard_,
                                   in_flight = in_flight_] {
                    deliverOnLoop(loop, hook, event_id, body, timeout_s, ssrf_guard, in_flight);
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
    if (!loop_thread_) {
        LOG_INFO << "webhook dispatcher stopped (loop never started)";
        return;  // start() не вызывали — сносить/дренировать нечего
    }
    auto* loop = loop_thread_->loop();

    // Барьер: воркер присоединён → новых доставок на loop не ставится. Ждём, пока loop прогонит
    // ВСЕ уже поставленные deliverOnLoop-функторы (FIFO) — только после этого in_flight_ отражает
    // все запущенные запросы (иначе гейт мог бы увидеть 0 при ещё не выполненном функторе и снести
    // loop под будущим sendRequest). Ограничен по времени: непрошедший барьер = loop не отвечает.
    bool loop_responsive = true;
    {
        auto barrier = std::make_shared<std::promise<void>>();
        auto fut = barrier->get_future();
        loop->queueInLoop([barrier] { barrier->set_value(); });
        if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
            loop_responsive = false;
        }
    }

    // Дренаж in-flight: ждём обнуления счётчика (drogon гарантирует колбэк по таймауту запроса,
    // так что счётчик обязан обнулиться в пределах ~timeout). Бюджет 2×timeout + запас.
    if (loop_responsive) {
        const auto budget =
            std::chrono::milliseconds(static_cast<long long>(timeout_s_ * 2000.0) + 500);
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (in_flight_->load(std::memory_order_acquire) > 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    const std::size_t remaining = in_flight_->load(std::memory_order_acquire);
    if (loop_responsive && remaining == 0) {
        // Гейт открыт: коннектов нет, все колбэки уничтожены. Штатный снос (quit+join).
        loop_thread_.reset();
    } else {
        // Снос loop'а под живым HTTP или на зависшем loop'е = тот же класс heap-corruption, что в
        // s3_client.cpp. Намеренно лизим loop-поток (release, без quit/join): счётчик, client и
        // токен живут в shared_ptr — колбэки доработают сами. Один поток на инцидент; процесс всё
        // равно завершается. НЕ join'им зависший поток (как retire в s3).
        LOG_ERROR << "webhook loop not drained at shutdown (in_flight=" << remaining
                  << " responsive=" << (loop_responsive ? 1 : 0)
                  << "): leaking loop thread to avoid teardown under live HTTP";
        (void)loop_thread_.release();
    }
    LOG_INFO << "webhook dispatcher stopped";
}

}  // namespace tgw::webhook
