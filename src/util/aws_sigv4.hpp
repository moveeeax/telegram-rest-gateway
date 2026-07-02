#pragma once

#include <map>
#include <string>
#include <string_view>

namespace tgw::util {

// Минимальный AWS Signature V4 для S3 (заголовочная авторизация, single-chunk payload).
// Вынесен отдельно от HTTP ради тестируемости против официальных AWS-векторов.

std::string sha256Hex(std::string_view data);
std::string hmacSha256(std::string_view key, std::string_view data);  // сырые байты

// Возвращает значение заголовка Authorization для запроса. amz_date — формат YYYYMMDDToHHMMSSZ.
// canonical_uri — уже URL-энкоденный путь (сегменты). query — уже отсортированный canonical
// query string (может быть пустой). signed_headers/canonical_headers строятся из headers:
// в подпись идут host, x-amz-content-sha256, x-amz-date (плюс всё, что передано в headers).
std::string sigv4Authorization(const std::string& method, const std::string& canonical_uri,
                               const std::string& canonical_query,
                               const std::map<std::string, std::string>& headers,
                               const std::string& payload_sha256_hex, const std::string& amz_date,
                               const std::string& region, const std::string& service,
                               const std::string& access_key, const std::string& secret_key);

}  // namespace tgw::util
