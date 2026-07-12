#include "util/s3_client.hpp"

#include "util/aws_sigv4.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/utils/Logger.h>

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
// Контекст НИКОГДА не разрушается (new без delete — намеренно): деструктор EventLoopThread
// делает quit()+join, а на выходе процесса detached-поток s3-sync может ещё держать
// in-flight PUT (main ждёт ~10 с, send() блокируется до 35 с) — это был бы тот же
// teardown-под-живым-запросом, только при static destruction. Если loop завис (send() вышел
// по 35-секундной страховке), контекст списывается (retireS3Io) и следующий запрос получает
// свежий; старый утекает — один поток на инцидент, зато S3 не отравлен навсегда и никто
// не делает join зависшего потока.
struct S3Io {
    trantor::EventLoopThread thread{"s3-client"};
    std::mutex mu;
    std::map<std::string, drogon::HttpClientPtr> clients;  // key: base URL endpoint'а
};

std::mutex g_s3_io_mu;
S3Io* g_s3_io = nullptr;

S3Io* acquireS3Io() {
    const std::lock_guard lock(g_s3_io_mu);
    if (g_s3_io == nullptr) {
        g_s3_io = new S3Io;
        g_s3_io->thread.run();
    }
    return g_s3_io;
}

// Списывает зависший контекст. Сравнение по указателю — чтобы два конкурентных таймаута
// не списали и свежий контекст тоже.
void retireS3Io(S3Io* io) {
    const std::lock_guard lock(g_s3_io_mu);
    if (g_s3_io == io) {
        g_s3_io = nullptr;
    }
}

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
    // зависит от жизненного цикла главного loop'а. HttpClient кэшируется по endpoint'у:
    // keep-alive-соединение вместо TCP+TLS-рукопожатия на каждый запрос.
    S3Io* io = acquireS3Io();
    drogon::HttpClientPtr client;
    {
        const std::lock_guard lock(io->mu);
        auto& cached = io->clients[ep.base];
        if (cached == nullptr) {
            cached = drogon::HttpClient::newHttpClient(ep.base, io->thread.getLoop());
        }
        client = cached;
    }

    // promise держим в shared_ptr: колбэк исполняется на s3-loop и обязан пережить in-flight
    // запрос, даже если send() уже вышел по таймауту. client в колбэк НЕ захватываем — его
    // держит io->clients (иначе цикл владения: HttpClient хранит колбэк, владеющий им самим,
    // и при невызванном колбэке клиент с копией binlog в теле запроса тёк бы на каждой попытке).
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
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
        30.0);

    // Страховка: 30s — таймаут Drogon (колбэк придёт с ReqResult::Timeout), +5s запас.
    // Drogon гарантирует вызов колбэка по таймауту, так что future всегда становится готовым.
    Result out;
    if (future.wait_for(std::chrono::seconds(35)) == std::future_status::timeout) {
        // Сюда попадаем только если loop-поток завис: Drogon обязан был отдать колбэк на 30 с.
        // Списываем контекст — следующий запрос пойдёт на свежем loop'е, а не в зависший.
        retireS3Io(io);
        LOG_ERROR << "s3 event loop hung (no callback in 35s): retiring io context, "
                     "one thread intentionally leaked";
        out = Result{0, "", "s3 request timed out (event loop hung)"};
    } else {
        out = future.get();
    }
    return out;
} catch (const std::exception& e) {
    return Result{0, "", std::string("s3 request build failed: ") + e.what()};
}

void S3Client::shutdownIdleLoop() {
    S3Io* io = nullptr;
    {
        const std::lock_guard lock(g_s3_io_mu);
        io = g_s3_io;
        g_s3_io = nullptr;
    }
    // Порядок разрушения членов S3Io (обратный объявлению) здесь важен: сначала clients
    // (HttpClient'ам ещё нужен живой loop), потом thread (~EventLoopThread = quit+join).
    delete io;
}

S3Client::Result S3Client::get() const {
    return send("GET", "");
}

S3Client::Result S3Client::put(std::string_view body) const {
    return send("PUT", body);
}

}  // namespace tgw::util
