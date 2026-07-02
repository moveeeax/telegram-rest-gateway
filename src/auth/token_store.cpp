#include "auth/token_store.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace tgw::auth {
namespace {

std::array<unsigned char, 32> sha256(std::string_view input) {
    std::array<unsigned char, 32> out{};
    unsigned int len = 0;
    EVP_Digest(input.data(), input.size(), out.data(), &len, EVP_sha256(), nullptr);
    return out;
}

}  // namespace

TokenStore& TokenStore::instance() {
    static TokenStore store;
    return store;
}

void TokenStore::load(const std::vector<std::string>& tokens) {
    hashes_.clear();
    hashes_.reserve(tokens.size());
    for (const auto& token : tokens) {
        hashes_.push_back(sha256(token));
    }
}

bool TokenStore::verify(std::string_view presented) const {
    const std::array<unsigned char, 32> hash = sha256(presented);
    int matched = 0;
    // Без раннего выхода: постоянное по количеству токенов время, timing-safe сравнение.
    for (const auto& stored : hashes_) {
        matched |= (CRYPTO_memcmp(hash.data(), stored.data(), hash.size()) == 0) ? 1 : 0;
    }
    return matched != 0;
}

}  // namespace tgw::auth
