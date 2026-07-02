#pragma once

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

   private:
    Result send(const std::string& method, std::string_view body) const;
    S3Config config_;
};

}  // namespace tgw::util
