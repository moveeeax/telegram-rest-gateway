#include "http/bearer_auth.hpp"

#include "auth/token_store.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string_view>

using tgw::auth::kAllScopes;
using tgw::auth::Scope;
using tgw::auth::ScopeMask;
using tgw::http::BearerAuthResult;
using tgw::http::evaluateBearerAuth;
using tgw::http::parseBearerToken;

namespace {

constexpr ScopeMask kReadWrite = static_cast<ScopeMask>(static_cast<ScopeMask>(Scope::Read) |
                                                        static_cast<ScopeMask>(Scope::Write));

// Фейковый verify: "tgw_admin" — все скоупы, "tgw_rw" — read+write, прочее — неизвестно.
std::optional<ScopeMask> fakeVerify(std::string_view token) {
    if (token == "tgw_admin") {
        return kAllScopes;
    }
    if (token == "tgw_rw") {
        return kReadWrite;
    }
    return std::nullopt;
}

}  // namespace

// --- parseBearerToken: разбор заголовка Authorization ---

TEST(ParseBearerToken, MissingHeaderRejected) {
    EXPECT_FALSE(parseBearerToken("").has_value());
}

TEST(ParseBearerToken, LowercasePrefixRejected) {
    EXPECT_FALSE(parseBearerToken("bearer token").has_value());  // регистр префикса важен
}

TEST(ParseBearerToken, EmptyTokenRejected) {
    EXPECT_FALSE(parseBearerToken("Bearer ").has_value());  // ровно префикс, токена нет
    EXPECT_FALSE(parseBearerToken("Bearer").has_value());  // короче префикса
}

TEST(ParseBearerToken, ValidTokenExtracted) {
    const auto token = parseBearerToken("Bearer abc123");
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, "abc123");
}

// Пробелы НЕ обрезаются: лишний пробел уходит в токен и делает его невалидным (позже -> 401).
TEST(ParseBearerToken, ExtraSpaceKeptInToken) {
    const auto token = parseBearerToken("Bearer  abc123");
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, " abc123");
}

// --- evaluateBearerAuth: решение 401 / 403 / next ---

TEST(EvaluateBearerAuth, MissingHeaderIsUnauthenticated) {
    EXPECT_EQ(evaluateBearerAuth("", Scope::Read, fakeVerify), BearerAuthResult::Unauthenticated);
}

TEST(EvaluateBearerAuth, LowercasePrefixIsUnauthenticated) {
    EXPECT_EQ(evaluateBearerAuth("bearer tgw_admin", Scope::Read, fakeVerify),
              BearerAuthResult::Unauthenticated);
}

TEST(EvaluateBearerAuth, UnknownTokenIsUnauthenticated) {
    EXPECT_EQ(evaluateBearerAuth("Bearer nope", Scope::Read, fakeVerify),
              BearerAuthResult::Unauthenticated);
}

// Лишний пробел -> токен " tgw_admin" не совпадёт с зарегистрированным -> 401.
TEST(EvaluateBearerAuth, ExtraSpaceTokenIsUnauthenticated) {
    EXPECT_EQ(evaluateBearerAuth("Bearer  tgw_admin", Scope::Read, fakeVerify),
              BearerAuthResult::Unauthenticated);
}

TEST(EvaluateBearerAuth, ValidTokenWithScopeAllowed) {
    EXPECT_EQ(evaluateBearerAuth("Bearer tgw_admin", Scope::Admin, fakeVerify),
              BearerAuthResult::Allowed);
    EXPECT_EQ(evaluateBearerAuth("Bearer tgw_rw", Scope::Write, fakeVerify),
              BearerAuthResult::Allowed);
}

// Валидный токен без нужного скоупа -> 403 (не 401): аутентифицирован, но не авторизован.
TEST(EvaluateBearerAuth, ValidTokenWithoutScopeIsForbidden) {
    EXPECT_EQ(evaluateBearerAuth("Bearer tgw_rw", Scope::Admin, fakeVerify),
              BearerAuthResult::Forbidden);
}
