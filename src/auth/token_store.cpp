#include "auth/token_store.hpp"

#include <trantor/utils/Logger.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <sstream>

namespace tgw::auth {
namespace {

std::array<unsigned char, 32> sha256(std::string_view input) {
    std::array<unsigned char, 32> out{};
    unsigned int len = 0;
    EVP_Digest(input.data(), input.size(), out.data(), &len, EVP_sha256(), nullptr);
    return out;
}

// "read,write" -> маска. Неизвестный скоуп игнорируется с предупреждением (ошибка в скоупе
// сузит права токена, не расширит).
ScopeMask parseScopes(const std::string& spec) {
    ScopeMask mask = 0;
    std::istringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item == "read") {
            mask |= static_cast<ScopeMask>(Scope::Read);
        } else if (item == "write") {
            mask |= static_cast<ScopeMask>(Scope::Write);
        } else if (item == "admin") {
            mask |= static_cast<ScopeMask>(Scope::Admin);
        } else if (!item.empty()) {
            LOG_WARN << "unknown token scope '" << item << "' ignored";
        }
    }
    return mask;
}

}  // namespace

TokenStore& TokenStore::instance() {
    static TokenStore store;
    return store;
}

void TokenStore::load(const std::vector<std::string>& entries) {
    entries_.clear();
    entries_.reserve(entries.size());
    for (const auto& line : entries) {
        // "<token>[ <scope,scope>]" — токен до первого пробела, дальше скоупы.
        const auto space = line.find(' ');
        const std::string token = line.substr(0, space);
        if (token.empty()) {
            continue;
        }
        ScopeMask scopes = kAllScopes;
        if (space != std::string::npos) {
            std::string spec = line.substr(space + 1);
            spec.erase(0, spec.find_first_not_of(" \t"));
            if (!spec.empty()) {
                scopes = parseScopes(spec);
            }
        }
        entries_.push_back(Entry{sha256(token), scopes});
    }
}

std::optional<ScopeMask> TokenStore::verify(std::string_view presented) const {
    const std::array<unsigned char, 32> hash = sha256(presented);
    int matched = 0;
    ScopeMask scopes = 0;
    // Без раннего выхода: постоянное по количеству токенов время, timing-safe сравнение.
    for (const auto& entry : entries_) {
        const int hit = (CRYPTO_memcmp(hash.data(), entry.hash.data(), hash.size()) == 0) ? 1 : 0;
        matched |= hit;
        scopes |= static_cast<ScopeMask>(hit != 0 ? entry.scopes : 0);
    }
    if (matched == 0) {
        return std::nullopt;
    }
    return scopes;
}

}  // namespace tgw::auth
