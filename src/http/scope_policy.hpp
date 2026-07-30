#pragma once

#include "auth/token_store.hpp"

#include <cstddef>
#include <string_view>

namespace tgw::http {

// Политика «какой скоуп нужен маршруту», вынесенная из BearerAuthFilter отдельно от
// drogon-типов — чтобы её можно было покрыть юнит-тестом (tests/scope_policy_test.cpp).
//
// ВАЖНО (регистр): Drogon матчит маршруты БЕЗ учёта регистра — HttpControllersRouter
// приводит и зарегистрированный путь, и req->path() к нижнему регистру (а regex-маршруты
// компилирует с std::regex_constants::icase). Значит `GET /V1/Auth/session/export`
// доходит до обработчика экспорта сессии. Регистрозависимая проверка префикса считала бы
// такой запрос обычным read-запросом и пускала бы read-токен к session string = полный
// захват Telegram-аккаунта. Поэтому префикс сравниваем без учёта регистра — ровно так же,
// как это делает роутер.
//
// req->path() приходит уже url-декодированным (HttpRequestImpl::setPath), поэтому
// процентное кодирование префикс не спрячет: роутер и эта проверка видят одну строку.
inline constexpr std::string_view kAuthPathPrefix = "/v1/auth/";
// Без завершающего слэша: под префикс должны попадать И ровно "/v1/webhooks" (GET/POST списка и
// создания), И "/v1/webhooks/{id}" (DELETE) — см. isWebhooksPath ниже, где это учтено явно.
inline constexpr std::string_view kWebhooksPathPrefix = "/v1/webhooks";

constexpr char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// true, если path (без учёта регистра, ASCII) начинается с prefix. prefix — литерал в нижнем
// регистре (сравнивается лишь символ пути, как и раньше в isAuthPath).
constexpr bool startsWithNoCase(std::string_view path, std::string_view prefix) {
    if (path.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (asciiLower(path[i]) != prefix[i]) {
            return false;
        }
    }
    return true;
}

// true, если путь принадлежит /v1/auth/* (сравнение без учёта регистра, ASCII).
constexpr bool isAuthPath(std::string_view path) {
    return startsWithNoCase(path, kAuthPathPrefix);
}

// true для ровно "/v1/webhooks" (список/создание) и "/v1/webhooks/..." (напр. DELETE .../{id}),
// без учёта регистра. Отдельная граничная проверка нужна: kWebhooksPathPrefix без слэша, иначе
// префиксным совпадением ложно захватился бы, например, гипотетический "/v1/webhooksomething".
constexpr bool isWebhooksPath(std::string_view path) {
    if (!startsWithNoCase(path, kWebhooksPathPrefix)) {
        return false;
    }
    return path.size() == kWebhooksPathPrefix.size() || path[kWebhooksPathPrefix.size()] == '/';
}

// Требуемый скоуп по маршруту: /v1/auth/* — admin (логин, session export = захват аккаунта);
// /v1/webhooks* — тоже admin (Task 7: список/секреты вебхуков определяют, куда утечёт контент
// чужих сообщений); GET/HEAD прочих путей — read; остальные методы (мутации) — write.
// path — url-декодированный req->path(); is_read_method — метод GET или HEAD.
constexpr tgw::auth::Scope requiredScopeFor(std::string_view path, bool is_read_method) {
    if (isAuthPath(path) || isWebhooksPath(path)) {
        return tgw::auth::Scope::Admin;
    }
    return is_read_method ? tgw::auth::Scope::Read : tgw::auth::Scope::Write;
}

}  // namespace tgw::http
