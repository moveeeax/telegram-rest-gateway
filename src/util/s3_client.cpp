#include "util/s3_client.hpp"

#include "util/aws_sigv4.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <future>
#include <map>
#include <mutex>
#include <string>

namespace tgw::util {
namespace {

std::string uriEncodePath(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : path) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~' || c == '/';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string amzDateNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

// Разбирает endpoint в scheme://host[:port] и отдельно host[:port] (для Host-заголовка).
struct Endpoint {
    std::string base;      // http(s)://host[:port]
    std::string hostport;  // host[:port]
};

Endpoint parseEndpoint(const std::string& endpoint, const std::string& vhost_prefix) {
    const auto scheme_end = endpoint.find("://");
    const std::string scheme =
        (scheme_end == std::string::npos) ? "https" : endpoint.substr(0, scheme_end);
    std::string rest =
        (scheme_end == std::string::npos) ? endpoint : endpoint.substr(scheme_end + 3);
    const auto slash = rest.find('/');
    if (slash != std::string::npos) {
        rest = rest.substr(0, slash);  // host[:port], путь игнорируем
    }
    const std::string hostport = vhost_prefix.empty() ? rest : (vhost_prefix + "." + rest);
    return Endpoint{scheme + "://" + hostport, hostport};
}

// Один долгоживущий IO-контекст (loop-поток + кэш HttpClient по endpoint'у) на ВСЕ
// S3-запросы. Раньше на каждый запрос создавался и тут же сносился отдельный
// EventLoopThread — teardown loop'а при ещё живом HttpClient / in-flight запросе портил
// кучу (heap corruption "malloc(): unsorted double linked list corrupted", креши на amd64).
// Собственный loop (независимый от главного loop'а Drogon) остаётся доступным и до app().run(),
// и после остановки — как и требуется для restore на старте и push на shutdown.
//
// Жизненный цикл:
//  - Не храним EventLoopThread в static storage: ~EventLoopThread = quit()+join, а на выходе
//    процесса фоновый s3-sync может ещё держать in-flight PUT — это был бы teardown под
//    живым запросом при static destruction.
//  - Штатный delete — только shutdownIdleLoop() при g_s3_in_flight == 0 (гарантированно idle).
//  - При зависании loop'а (send() вышел по 35s-страховке) контекст списывается (retireS3Io)
//    без delete: старый утекает (один поток на инцидент), следующий запрос получает свежий.
//    Никто не join'ит зависший поток.
struct S3Io {
    trantor::EventLoopThread thread{"s3-client"};
    std::mutex mu;
    // Кэшированный клиент (keep-alive): endpoint в процессе один (всегда из config.s3),
    // поэтому один слот, а не map. client_base — base URL, для которого клиент создан.
    drogon::HttpClientPtr client;
    std::string client_base;
};

std::mutex g_s3_io_mu;
S3Io* g_s3_io = nullptr;
// true после первого retire: где-то жив утёкший зависший поток. Никогда не сбрасывается —
// с этого момента shutdownIdleLoop обязан сообщать "unclean" (выход только через _Exit).
bool g_s3_ever_retired = false;
// Число активных send(): acquire под g_s3_io_mu, release после завершения wait/таймаута.
// shutdownIdleLoop no-op'ит, пока счётчик > 0 — иначе UAF на raw S3Io*.
std::atomic<int> g_s3_in_flight{0};

// Берёт (или создаёт) контекст и помечает in-flight. Пара: releaseS3IoSend в finally.
S3Io* acquireS3IoForSend() {
    const std::lock_guard lock(g_s3_io_mu);
    if (g_s3_io == nullptr) {
        g_s3_io = new S3Io;
        g_s3_io->thread.run();
    }
    g_s3_in_flight.fetch_add(1, std::memory_order_acq_rel);
    return g_s3_io;
}

void releaseS3IoSend() {
    g_s3_in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

// Списывает зависший контекст. Сравнение по указателю — чтобы два конкурентных таймаута
// не списали и свежий контекст тоже. delete не делаем (leak by design).
void retireS3Io(S3Io* io) {
    const std::lock_guard lock(g_s3_io_mu);
    g_s3_ever_retired = true;
    if (g_s3_io == io) {
        g_s3_io = nullptr;
    }
}

struct S3InFlightGuard {
    ~S3InFlightGuard() { releaseS3IoSend(); }
};

}  // namespace

S3Client::Result S3Client::send(const std::string& method, std::string_view body) const try {
    const std::string vhost_prefix = config_.path_style ? "" : config_.bucket;
    const Endpoint ep = parseEndpoint(config_.endpoint, vhost_prefix);

    const std::string object_path = "/" + config_.key;
    const std::string canonical_uri =
        uriEncodePath(config_.path_style ? ("/" + config_.bucket + object_path) : object_path);

    const std::string payload_hash = sha256Hex(body);
    const std::string amz_date = amzDateNow();

    std::map<std::string, std::string> headers;
    headers["host"] = ep.hostport;
    headers["x-amz-content-sha256"] = payload_hash;
    headers["x-amz-date"] = amz_date;

    const std::string authorization =
        sigv4Authorization(method, canonical_uri, "", headers, payload_hash, amz_date,
                           config_.region, "s3", config_.access_key, config_.secret_key);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method == "PUT" ? drogon::Put : drogon::Get);
    req->setPath(canonical_uri);
    req->addHeader("x-amz-content-sha256", payload_hash);
    req->addHeader("x-amz-date", amz_date);
    req->addHeader("Authorization", authorization);
    if (method == "PUT") {
        req->setBody(std::string(body));
    }

    // send() вызывается на старте (до app().run()) и на shutdown (после остановки главного
    // loop'а Drogon). Крутим запрос на собственном долгоживущем loop-потоке, который не
    // зависит от жизненного цикла главного loop'а. HttpClient кэшируется:
    // keep-alive-соединение вместо TCP+TLS-рукопожатия на каждый запрос.
    S3Io* io = acquireS3IoForSend();
    const S3InFlightGuard in_flight_guard;

    // До двух попыток: кэшированное keep-alive-соединение могло протухнуть по idle-таймауту
    // сервера (S3/MinIO часто < интервала синка) — сервер закрывает сокет, когда мы уже пишем
    // запрос, drogon отдаёт транспортную ошибку без ретрая. Такая ошибка проявляется быстро;
    // медленные ошибки (30s-таймаут Drogon) НЕ ретраим, чтобы не удваивать бюджет shutdown.
    Result out;
    for (int attempt = 0; attempt < 2; ++attempt) {
        drogon::HttpClientPtr client;
        {
            const std::lock_guard lock(io->mu);
            if (io->client == nullptr || io->client_base != ep.base) {
                io->client = drogon::HttpClient::newHttpClient(ep.base, io->thread.getLoop());
                io->client_base = ep.base;
            }
            client = io->client;
        }

        // promise держим в shared_ptr: колбэк исполняется на s3-loop и обязан пережить in-flight
        // запрос, даже если send() уже вышел по таймауту.
        // client в колбэк НЕ захватываем: (1) цикл владения HttpClient↔callback при невызванном
        // колбэке держал бы копию binlog в теле PUT на каждой попытке; (2) lifetime на время
        // запроса уже обеспечен Drogon (RequestCallbackParams::clientPtr) и кэшем io->client.
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        const auto started = std::chrono::steady_clock::now();
        client->sendRequest(
            req,
            [promise](drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
                if (result != drogon::ReqResult::Ok || resp == nullptr) {
                    promise->set_value(Result{
                        0, "", "s3 transport error: " + std::to_string(static_cast<int>(result))});
                    return;
                }
                promise->set_value(Result{resp->statusCode(), std::string(resp->body()), ""});
            },
            static_cast<double>(kRequestTimeout.count()));

        // Страховка: kRequestTimeout — таймаут Drogon (колбэк придёт с ReqResult::Timeout),
        // kSendHangTimeout = +5s запас. Drogon гарантирует вызов колбэка по таймауту, так что
        // future всегда становится готовым. Если loop завис — колбэк не придёт, wait_for
        // выйдет по kSendHangTimeout, контекст списываем.
        if (future.wait_for(kSendHangTimeout) == std::future_status::timeout) {
            // Сюда попадаем только если loop-поток завис: Drogon обязан был отдать колбэк.
            // Списываем контекст — следующий запрос пойдёт на свежем loop'е, а не в зависший.
            retireS3Io(io);
            LOG_ERROR << "s3 event loop hung (no callback in " << kSendHangTimeout.count()
                      << "s): retiring io context, one thread intentionally leaked";
            if (method == "PUT") {
                // Брошенный PUT живёт в ядре/зависшем loop'е и может закоммититься в S3 ПОЗЖЕ
                // более свежего снапшота (last-writer-wins). Изнутри процесса это не отменить —
                // защищает только versioning бакета.
                LOG_ERROR << "abandoned s3 PUT may still complete later and overwrite a newer "
                             "snapshot — enable bucket versioning to be safe";
            }
            return Result{0, "", "s3 request timed out (event loop hung)"};
        }
        out = future.get();

        const bool transport_error = (out.http_status == 0);
        const bool fast_failure =
            (std::chrono::steady_clock::now() - started) < std::chrono::seconds(5);
        if (!transport_error || !fast_failure || attempt == 1) {
            break;
        }
        // Сбрасываем кэшированный клиент (если его никто не заменил) и повторяем один раз.
        {
            const std::lock_guard lock(io->mu);
            if (io->client == client) {
                io->client = nullptr;
            }
        }
        LOG_WARN << "s3 transport error on cached connection (" << out.error
                 << ") — retrying once on a fresh client";
    }
    return out;
} catch (const std::exception& e) {
    return Result{0, "", std::string("s3 request build failed: ") + e.what()};
}

bool S3Client::shutdownIdleLoop() {
    S3Io* io = nullptr;
    bool clean = false;
    {
        const std::lock_guard lock(g_s3_io_mu);
        if (g_s3_in_flight.load(std::memory_order_acquire) != 0) {
            LOG_WARN << "s3 shutdownIdleLoop: in-flight requests present — refusing teardown";
            return false;
        }
        io = g_s3_io;
        g_s3_io = nullptr;
        // Даже если текущий контекст гасится штатно, ранее списанный зависший поток всё ещё
        // жив — выход через static destruction небезопасен.
        clean = !g_s3_ever_retired;
    }
    // Порядок разрушения членов S3Io (обратный объявлению) здесь важен: сначала client
    // (HttpClient'у ещё нужен живой loop), потом thread (~EventLoopThread = quit+join).
    delete io;
    return clean;
}

S3Client::Result S3Client::get() const {
    return send("GET", "");
}

S3Client::Result S3Client::put(std::string_view body) const {
    return send("PUT", body);
}

}  // namespace tgw::util
