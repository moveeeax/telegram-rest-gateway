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

constexpr char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// true, если путь принадлежит /v1/auth/* (сравнение без учёта регистра, ASCII).
constexpr bool isAuthPath(std::string_view path) {
    if (path.size() < kAuthPathPrefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kAuthPathPrefix.size(); ++i) {
        if (asciiLower(path[i]) != kAuthPathPrefix[i]) {
            return false;
        }
    }
    return true;
}

// Требуемый скоуп по маршруту: /v1/auth/* — admin (логин, session export = захват аккаунта);
// GET/HEAD — read; остальные методы (мутации) — write.
// path — url-декодированный req->path(); is_read_method — метод GET или HEAD.
constexpr tgw::auth::Scope requiredScopeFor(std::string_view path, bool is_read_method) {
    if (isAuthPath(path)) {
        return tgw::auth::Scope::Admin;
    }
    return is_read_method ? tgw::auth::Scope::Read : tgw::auth::Scope::Write;
}

}  // namespace tgw::http
