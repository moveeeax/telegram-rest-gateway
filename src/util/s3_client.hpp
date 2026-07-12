#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace tgw::util {

struct S3Config {
    std::string endpoint;  // http(s)://host[:port] (AWS: https://s3.<region>.amazonaws.com)
    std::string region;
    std::string bucket;
    std::string key;  // путь объекта, напр. sessions/telegram/binlog
    std::string access_key;
    std::string secret_key;
    bool path_style = true;  // true для MinIO/совместимых; false — virtual-host для AWS

    bool enabled() const {
        return !bucket.empty() && !key.empty() && !access_key.empty() && !secret_key.empty() &&
               !endpoint.empty();
    }
};

// Тонкий S3-клиент: только GET/PUT объекта по ключу конфига. AWS SigV4 (util/aws_sigv4).
// Синхронный (Drogon HttpClient::sendRequest) — вызывается на старте/shutdown.
class S3Client {
   public:
    struct Result {
        int http_status = 0;  // 0 — транспортная ошибка (см. error)
        std::string body;  // тело ответа (для GET — содержимое объекта)
        std::string error;  // непусто при транспортной ошибке
        bool ok() const { return http_status >= 200 && http_status < 300; }
        bool notFound() const { return http_status == 404; }
    };

    explicit S3Client(S3Config config) : config_(std::move(config)) {}

    Result get() const;
    Result put(std::string_view body) const;

    // Таймауты send(): kRequestTimeout — таймаут Drogon (колбэк гарантирован),
    // kSendHangTimeout — страховка wait_for на случай зависшего loop'а (+5s запас).
    // Внешние ожидания (shutdown в main) должны отталкиваться от kSendHangTimeout,
    // а не дублировать число.
    static constexpr std::chrono::seconds kRequestTimeout{30};
    static constexpr std::chrono::seconds kSendHangTimeout{35};

    // Штатно гасит общий s3 loop-поток (quit+join). Безопасно: внутри есть in-flight guard —
    // при активных send() отказ (лог WARN), без UAF. Идеальный call site — после финального
    // push, когда in-flight уже 0. Возвращает true, если s3-потоков гарантированно не осталось
    // (loop не создавался или штатно погашен); false — жив активный send() или списанный
    // зависший поток: вызывающему НЕЛЬЗЯ уходить в static destruction (return из main) —
    // выходить через _Exit.
    static bool shutdownIdleLoop();

   private:
    Result send(const std::string& method, std::string_view body) const;
    S3Config config_;
};

}  // namespace tgw::util
