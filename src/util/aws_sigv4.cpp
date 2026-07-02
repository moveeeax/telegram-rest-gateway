#include "util/aws_sigv4.hpp"

#include <array>
#include <cstdio>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdexcept>

namespace tgw::util {
namespace {

std::string toHex(const unsigned char* data, std::size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

}  // namespace

std::string sha256Hex(std::string_view data) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    unsigned int len = 0;
    // При ошибке кидаем — иначе хэш нулевого блока молча даст неверную SigV4-подпись (→ 403).
    if (EVP_Digest(data.data(), data.size(), digest.data(), &len, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("EVP_Digest(sha256) failed");
    }
    return toHex(digest.data(), digest.size());
}

std::string hmacSha256(std::string_view key, std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    unsigned int len = 0;
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(), out.data(),
             &len) == nullptr) {
        throw std::runtime_error("HMAC(sha256) failed");
    }
    return std::string(reinterpret_cast<const char*>(out.data()), len);
}

std::string sigv4Authorization(const std::string& method, const std::string& canonical_uri,
                               const std::string& canonical_query,
                               const std::map<std::string, std::string>& headers,
                               const std::string& payload_sha256_hex, const std::string& amz_date,
                               const std::string& region, const std::string& service,
                               const std::string& access_key, const std::string& secret_key) {
    // Канонические заголовки: имена в lower-case, отсортированы (std::map уже сортирует).
    std::string canonical_headers;
    std::string signed_headers;
    for (const auto& [name, value] : headers) {
        canonical_headers += name + ":" + value + "\n";
        if (!signed_headers.empty()) {
            signed_headers += ";";
        }
        signed_headers += name;
    }

    const std::string canonical_request = method + "\n" + canonical_uri + "\n" + canonical_query +
                                          "\n" + canonical_headers + "\n" + signed_headers + "\n" +
                                          payload_sha256_hex;

    const std::string date_stamp = amz_date.substr(0, 8);  // YYYYMMDD
    const std::string scope = date_stamp + "/" + region + "/" + service + "/aws4_request";
    const std::string string_to_sign =
        "AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" + sha256Hex(canonical_request);

    // Цепочка ключей подписи.
    const std::string k_date = hmacSha256("AWS4" + secret_key, date_stamp);
    const std::string k_region = hmacSha256(k_date, region);
    const std::string k_service = hmacSha256(k_region, service);
    const std::string k_signing = hmacSha256(k_service, "aws4_request");
    const std::string signature_raw = hmacSha256(k_signing, string_to_sign);
    const std::string signature =
        toHex(reinterpret_cast<const unsigned char*>(signature_raw.data()), signature_raw.size());

    return "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
           ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
}

}  // namespace tgw::util
