#pragma once

#include "auth/token_store.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// Разбор заголовка Authorization и решение 401-vs-403, вынесенные из BearerAuthFilter
// (решение 3.6) отдельно от drogon-типов — по образцу http/scope_policy.hpp. Так чистую
// логику фильтра можно покрыть юнит-тестом (tests/bearer_auth_test.cpp), а bearer_filter.cpp
// остаётся тонкой прослойкой «достать заголовок из req -> вызвать это -> сконвертировать исход
// в HTTP-ответ». Поведение бит-в-бит как было.
namespace tgw::http {

// Исход аутентификации: три ветви ровно соответствуют реакции фильтра.
enum class BearerAuthResult : std::uint8_t {
    Allowed,  // токен валиден и имеет требуемый скоуп -> next()
    Unauthenticated,  // нет заголовка / не тот префикс / пустой / неизвестный токен -> 401
    Forbidden,        // токен валиден, но не хватает скоупа -> 403
};

// Извлекает токен из заголовка Authorization: строго префикс "Bearer " (регистрозависимо,
// RFC 6750 §2.1) и непустой остаток. nullopt — заголовок не является валидным Bearer.
// Пробелы НЕ обрезаются: "Bearer  x" даёт токен " x" (с ведущим пробелом), который просто не
// совпадёт ни с одним зарегистрированным токеном => 401. Так же вёл себя исходный фильтр:
// проверка была header.size() > kPrefix.size() && header начинается с "Bearer ".
inline std::optional<std::string_view> parseBearerToken(std::string_view header) {
    constexpr std::string_view kPrefix = "Bearer ";
    // "" / "Bearer" / "Bearer " (пустой токен) — короче либо ровно равны префиксу.
    if (header.size() <= kPrefix.size()) {
        return std::nullopt;
    }
    if (header.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;  // не тот префикс (в т.ч. "bearer " в нижнем регистре)
    }
    return header.substr(kPrefix.size());
}

// Полное решение фильтра. verify(token) -> маска скоупов токена либо nullopt (токен неизвестен);
// вынесено параметром, чтобы тест подставлял фейковый verify без TokenStore-синглтона и drogon.
template <typename VerifyFn>
BearerAuthResult evaluateBearerAuth(std::string_view authorization_header,
                                    tgw::auth::Scope required, VerifyFn&& verify) {
    const std::optional<std::string_view> token = parseBearerToken(authorization_header);
    if (!token) {
        return BearerAuthResult::Unauthenticated;
    }
    const std::optional<tgw::auth::ScopeMask> scopes = verify(*token);
    if (!scopes) {
        return BearerAuthResult::Unauthenticated;  // токен не найден
    }
    if (!tgw::auth::scopeAllows(*scopes, required)) {
        return BearerAuthResult::Forbidden;
    }
    return BearerAuthResult::Allowed;
}

}  // namespace tgw::http
