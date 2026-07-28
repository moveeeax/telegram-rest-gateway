#pragma once

#include "bridge/td_bridge.hpp"

#include <td/telegram/td_api.h>

#include <drogon/drogon.h>

#include <json/value.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

// Общие хелперы HTTP-слоя (решение 1.6): раньше jsonResponse/serviceError/telegramError,
// parseId и launchInvoke жили копиями в routes.cpp, message_routes.cpp, directory_routes.cpp,
// bearer_filter.cpp, и их сигнатуры уже разъехались (const char* против const std::string&,
// parseId на std::stoll против std::strtoll). Здесь — единственный источник, все четыре файла
// переведены на него. Сюда же — маппинг ошибок TDLib в HTTP-статус (решение 1.4).
namespace tgw::http {

// Колбэк ответа Drogon и билдер ответа из объекта TDLib — общие для detached-корутин.
using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;
using ResponseBuilder =
    std::function<drogon::HttpResponsePtr(td::td_api::object_ptr<td::td_api::Object>)>;

namespace detail {

// ASCII-lower одного символа (без локали — регистр TDLib-сообщений всегда ASCII).
constexpr char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Индекс первого вхождения needle в haystack без учёта регистра (ASCII); npos — нет.
inline std::size_t findNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    if (haystack.size() < needle.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (asciiLower(haystack[i + j]) != asciiLower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return std::string_view::npos;
}

inline bool containsNoCase(std::string_view haystack, std::string_view needle) {
    return findNoCase(haystack, needle) != std::string_view::npos;
}

inline bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (asciiLower(s[i]) != asciiLower(prefix[i])) {
            return false;
        }
    }
    return true;
}

// Первое целое число, стоящее после позиции from (пропускаем всё до первой цифры). false —
// цифр нет либо переполнение (std::from_chars вернёт errc). out заполняется только при успехе.
inline bool parseTrailingNumber(std::string_view s, std::size_t from, long& out) {
    std::size_t i = from;
    while (i < s.size() && (s[i] < '0' || s[i] > '9')) {
        ++i;
    }
    const std::size_t start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
    }
    if (start == i) {
        return false;
    }
    long value = 0;
    const auto res = std::from_chars(s.data() + start, s.data() + i, value);
    if (res.ec != std::errc{}) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace detail

inline drogon::HttpResponsePtr jsonResponse(Json::Value body, drogon::HttpStatusCode code) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    resp->setStatusCode(code);
    return resp;
}

inline drogon::HttpResponsePtr serviceError(const std::string& code, const std::string& message,
                                            drogon::HttpStatusCode http) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = code;
    body["error"]["message"] = message;
    return jsonResponse(std::move(body), http);
}

// Маппинг ошибки TDLib в HTTP-статус (решение 1.4). Раньше ВСЕ ошибки TDLib отдавались как 502
// -> ложные 5xx-алерты и ретраи неидемпотентных операций балансировщиками на «Chat not found».
//   - code 400 + сообщение содержит "not found" (без учёта регистра) -> 404;
//   - иной code 400 -> 400;
//   - code 429 либо сообщение начинается с "Too Many Requests" / содержит FLOOD_WAIT -> 429;
//   - всё остальное (401/403/500 и пр. — проблемы сессии гейтвея, клиент их не чинит) -> 502.
inline drogon::HttpStatusCode httpStatusForTdError(const td::td_api::error& error) {
    if (error.code_ == 400) {
        return detail::containsNoCase(error.message_, "not found") ? drogon::k404NotFound
                                                                   : drogon::k400BadRequest;
    }
    if (error.code_ == 429 || detail::startsWithNoCase(error.message_, "Too Many Requests") ||
        detail::containsNoCase(error.message_, "FLOOD_WAIT")) {
        return drogon::k429TooManyRequests;
    }
    return drogon::k502BadGateway;
}

// Секунды для Retry-After из текста flood-ошибки TDLib ("Too Many Requests: retry after N",
// "FLOOD_WAIT_N"). Парсим консервативно: не распознали — false, вызывающий просто без заголовка.
inline bool retryAfterSeconds(const std::string& message, long& seconds) {
    std::size_t pos = detail::findNoCase(message, "retry after");
    if (pos != std::string_view::npos) {
        return detail::parseTrailingNumber(message, pos, seconds);
    }
    pos = detail::findNoCase(message, "FLOOD_WAIT_");
    if (pos != std::string_view::npos) {
        return detail::parseTrailingNumber(message, pos, seconds);
    }
    return false;
}

// Конверт ошибки TDLib (code: "TELEGRAM_ERROR", message + tdlib_code/tdlib_message) со статусом
// из httpStatusForTdError. При 429 добавляет Retry-After, если из текста извлекаются секунды.
inline drogon::HttpResponsePtr telegramError(const td::td_api::error& error) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = "TELEGRAM_ERROR";
    body["error"]["message"] = error.message_;
    body["error"]["tdlib_code"] = error.code_;
    body["error"]["tdlib_message"] = error.message_;
    const drogon::HttpStatusCode status = httpStatusForTdError(error);
    auto resp = jsonResponse(std::move(body), status);
    if (status == drogon::k429TooManyRequests) {
        long secs = 0;
        if (retryAfterSeconds(error.message_, secs)) {
            resp->addHeader("Retry-After", std::to_string(secs));
        }
    }
    return resp;
}

// Разбор строкового id (chat/message/file/...): строго весь текст — десятичное целое.
// Единый механизм для всех маршрутов (раньше std::stoll в одном файле и std::strtoll в другом).
inline bool parseId(const std::string& text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t pos = 0;
        out = std::stoll(text, &pos);
        return pos == text.size();
    } catch (...) {
        return false;
    }
}

// Общий запуск detached-корутины: co_await ответа TDLib, затем builder строит HTTP-ответ.
// registerHandler у Drogon не биндит корутинные лямбды, поэтому корутину гоняем внутри обычного
// callback-хендлера. Аргументы забираем по значению — корутина переживает возврат из хендлера.
inline void launchInvoke(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                         td::td_api::object_ptr<td::td_api::Function> fn, HttpCallback callback,
                         ResponseBuilder build) {
    [](tgw::bridge::TdBridge& td, std::int32_t cid,
       td::td_api::object_ptr<td::td_api::Function> f, HttpCallback cb,
       ResponseBuilder builder) -> drogon::AsyncTask {
        auto object = co_await td.invoke(cid, std::move(f));
        cb(builder(std::move(object)));
        co_return;
    }(bridge, client_id, std::move(fn), std::move(callback), std::move(build));
}

}  // namespace tgw::http
