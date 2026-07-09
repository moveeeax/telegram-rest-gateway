#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tgw::auth {

// Скоупы API-токенов (§8.1+): read — GET-эндпоинты и WS-поток; write — мутации
// (send/edit/delete/forward/react/upload/join); admin — /v1/auth/* (логин и, главное,
// session export = полный захват аккаунта). Токен без скоупов в конфиге = все три
// (обратная совместимость).
enum class Scope : std::uint8_t {
    Read = 1U << 0U,
    Write = 1U << 1U,
    Admin = 1U << 2U,
};
using ScopeMask = std::uint8_t;
constexpr ScopeMask kAllScopes = 0x7;

constexpr bool scopeAllows(ScopeMask mask, Scope s) {
    return (mask & static_cast<ScopeMask>(s)) != 0;
}

// Хранилище API-токенов. Формат строки BEARER_TOKENS:
//   "<token>"                  — полный доступ (read,write,admin)
//   "<token> read"             — только чтение (для агентов/дашбордов)
//   "<token> read,write"       — чтение и мутации, но без /v1/auth/*
// Хранит ТОЛЬКО SHA-256 токенов; сравнение — по всем записям без раннего выхода,
// CRYPTO_memcmp. Заполняется один раз на старте, далее только чтение.
class TokenStore {
   public:
    static TokenStore& instance();

    void load(const std::vector<std::string>& entries);
    // nullopt — токен неизвестен; иначе маска скоупов токена.
    std::optional<ScopeMask> verify(std::string_view presented) const;

    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

   private:
    struct Entry {
        std::array<unsigned char, 32> hash;
        ScopeMask scopes;
    };
    std::vector<Entry> entries_;
};

}  // namespace tgw::auth
