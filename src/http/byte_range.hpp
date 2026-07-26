#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace tgw::http {
namespace detail {

// Весь спан — непустая строка из ASCII-цифр (RFC 9110 §14.1.1: byte-range-spec = 1*DIGIT).
// std::stoull молча принимает "5abc" как 5 и игнорирует хвост — этим проверяем синтаксис ДО
// парсинга, а не полагаемся на него.
inline bool isAllDigits(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

// out заполняется только при успехе. false — не все символы цифры, либо переполнение
// std::uintmax_t (для реальных размеров файлов такое число всё равно означает "невалидно").
inline bool parseDigits(std::string_view s, std::uintmax_t& out) {
    if (!isAllDigits(s)) {
        return false;
    }
    try {
        out = std::stoull(std::string(s));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace detail

// Разбор одиночного "Range: bytes=a-b" (decision C6, GET /v1/files/{fileId}). Чистая функция —
// без drogon/TDLib, покрыта юнит-тестом (tests/byte_range_test.cpp).
//
// true — диапазон валиден, offset/length заполнены. false — заголовок отсутствует/не наш
// формат/multi-range/за границами файла/синтаксически невалиден: вызывающий код обязан
// ответить 416.
inline bool parseByteRange(std::string_view header, std::uintmax_t file_size,
                           std::uintmax_t& offset, std::uintmax_t& length) {
    constexpr std::string_view kPrefix = "bytes=";
    if (header.size() <= kPrefix.size() || header.compare(0, kPrefix.size(), kPrefix) != 0 ||
        file_size == 0) {
        return false;
    }
    const std::string_view spec = header.substr(kPrefix.size());
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos || spec.find(',') != std::string_view::npos) {
        return false;  // multi-range не поддерживаем
    }
    const std::string_view from = spec.substr(0, dash);
    const std::string_view to = spec.substr(dash + 1);

    if (from.empty()) {  // суффикс: bytes=-N (последние N байт)
        std::uintmax_t n = 0;
        if (!detail::parseDigits(to, n) || n == 0) {
            return false;
        }
        length = std::min<std::uintmax_t>(n, file_size);
        offset = file_size - length;
        return true;
    }

    if (!detail::parseDigits(from, offset) || offset >= file_size) {
        return false;
    }
    std::uintmax_t last = file_size - 1;
    if (!to.empty()) {
        std::uintmax_t to_val = 0;
        if (!detail::parseDigits(to, to_val)) {
            return false;
        }
        last = std::min<std::uintmax_t>(to_val, file_size - 1);
    }
    if (last < offset) {
        return false;
    }
    length = last - offset + 1;
    return true;
}

}  // namespace tgw::http
