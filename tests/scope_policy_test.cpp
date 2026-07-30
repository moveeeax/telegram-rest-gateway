#include "http/scope_policy.hpp"

#include <gtest/gtest.h>

using tgw::auth::Scope;
using tgw::http::requiredScopeFor;

TEST(ScopePolicy, AuthRoutesRequireAdmin) {
    EXPECT_EQ(requiredScopeFor("/v1/auth/session/export", true), Scope::Admin);
    EXPECT_EQ(requiredScopeFor("/v1/auth/state", true), Scope::Admin);
    EXPECT_EQ(requiredScopeFor("/v1/auth/password", false), Scope::Admin);
}

// РЕГРЕССИЯ: Drogon матчит маршруты без учёта регистра (HttpControllersRouter приводит
// req->path() и зарегистрированный путь к нижнему регистру), поэтому
// GET /V1/Auth/session/export попадает В ОБРАБОТЧИК экспорта сессии. Регистрозависимая
// проверка префикса выдавала бы для него Scope::Read — read-токен получал бы session string
// (полный доступ к Telegram-аккаунту).
TEST(ScopePolicy, AuthRoutesRequireAdminRegardlessOfCase) {
    EXPECT_EQ(requiredScopeFor("/V1/auth/session/export", true), Scope::Admin);
    EXPECT_EQ(requiredScopeFor("/v1/Auth/session/export", true), Scope::Admin);
    EXPECT_EQ(requiredScopeFor("/V1/AUTH/SESSION/EXPORT", true), Scope::Admin);
    EXPECT_EQ(requiredScopeFor("/v1/AuTh/code", false), Scope::Admin);
}

TEST(ScopePolicy, ReadMethodsNeedReadScope) {
    EXPECT_EQ(requiredScopeFor("/v1/chats", true), Scope::Read);
    EXPECT_EQ(requiredScopeFor("/v1/me", true), Scope::Read);
    EXPECT_EQ(requiredScopeFor("/v1/files/42", true), Scope::Read);
}

TEST(ScopePolicy, MutatingMethodsNeedWriteScope) {
    EXPECT_EQ(requiredScopeFor("/v1/chats/1/messages", false), Scope::Write);
    EXPECT_EQ(requiredScopeFor("/v1/chats/join", false), Scope::Write);
}

// Префикс — именно "/v1/auth/": похожие пути не должны внезапно требовать admin,
// а короткий "/v1/auth" (без слэша) не является префиксом.
TEST(ScopePolicy, NonAuthPathsAreNotEscalated) {
    EXPECT_EQ(requiredScopeFor("/v1/authorized", true), Scope::Read);
    EXPECT_EQ(requiredScopeFor("/v1/auth", true), Scope::Read);
    EXPECT_EQ(requiredScopeFor("/v1/au", true), Scope::Read);
    EXPECT_EQ(requiredScopeFor("", true), Scope::Read);
    EXPECT_EQ(requiredScopeFor("/metrics", true), Scope::Read);
}

// Task 7: /v1/webhooks* — admin, как и /v1/auth/*, для ОБОИХ форм пути: ровно "/v1/webhooks"
// (список/создание) и "/v1/webhooks/{id}" (удаление) — обе формы должны попасть под admin
// независимо от read/write метода.
TEST(ScopePolicy, WebhooksRoutesRequireAdmin) {
    EXPECT_EQ(requiredScopeFor("/v1/webhooks", true), Scope::Admin);   // GET (список)
    EXPECT_EQ(requiredScopeFor("/v1/webhooks", false), Scope::Admin);  // POST (создание)
    EXPECT_EQ(requiredScopeFor("/v1/webhooks/abc123", false), Scope::Admin);  // DELETE .../{id}
}

// Регистр не важен — тот же регресс-риск, что и для /v1/auth/* (роутер матчит без учёта
// регистра).
TEST(ScopePolicy, WebhooksRoutesRequireAdminRegardlessOfCase) {
    EXPECT_EQ(requiredScopeFor("/V1/Webhooks", true), Scope::Admin);
    EXPECT_EQ(requiredScopeFor("/v1/WEBHOOKS/abc123", false), Scope::Admin);
}

// Граница префикса: "/v1/webhooks" БЕЗ слэша/суффикса — совпадение (ровно список/создание), а
// похожий, но иной путь ("/v1/webhooksomething") не должен ложно эскалироваться до admin.
TEST(ScopePolicy, NonWebhooksPathsAreNotEscalated) {
    EXPECT_EQ(requiredScopeFor("/v1/webhooksomething", true), Scope::Read);
    EXPECT_EQ(requiredScopeFor("/v1/webhook", true), Scope::Read);
}
