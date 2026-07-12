#include "util/s3_client.hpp"

#include "util/aws_sigv4.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <trantor/net/EventLoopThread.h>

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

// Один долгоживущий loop-поток на ВСЕ S3-запросы. Создаётся лениво при первом вызове и
// живёт до конца процесса. Раньше на каждый запрос создавался и тут же сносился отдельный
// EventLoopThread — teardown loop'а при ещё живом HttpClient / in-flight запросе портил
// кучу (heap corruption "malloc(): unsorted double linked list corrupted", креши на amd64).
// Собственный loop (независимый от главного loop'а Drogon) остаётся доступным и до app().run(),
// и после остановки — как и требуется для restore на старте и push на shutdown.
trantor::EventLoop* s3Loop() {
    static trantor::EventLoopThread thread("s3-client");
    static std::once_flag once;
    std::call_once(once, [] { thread.run(); });
    return thread.getLoop();
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
    // loop'а Drogon). Крутим запрос на собственном долгоживущем loop-потоке (s3Loop), который
    // не зависит от жизненного цикла главного loop'а и НЕ пересоздаётся на каждый запрос.
    auto client = drogon::HttpClient::newHttpClient(ep.base, s3Loop());

    // promise и client держим в shared_ptr и захватываем в колбэк: колбэк исполняется на
    // s3-loop и обязан пережить in-flight запрос, даже если send() уже вышел по таймауту
    // (иначе HttpClient уничтожится из-под работающего на нём запроса).
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    client->sendRequest(
        req,
        [promise, client](drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
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
        out = Result{0, "", "s3 request timed out (event loop hung)"};
    } else {
        out = future.get();
    }
    // loop НЕ трогаем — он общий и живёт дальше; client уничтожится штатно (колбэк уже отработал).
    return out;
} catch (const std::exception& e) {
    return Result{0, "", std::string("s3 request build failed: ") + e.what()};
}

S3Client::Result S3Client::get() const {
    return send("GET", "");
}

S3Client::Result S3Client::put(std::string_view body) const {
    return send("PUT", body);
}

}  // namespace tgw::util
