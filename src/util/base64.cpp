#include "util/base64.hpp"

#include <array>
#include <cstdint>

namespace tgw::util {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::array<std::int8_t, 256> buildReverse() {
    std::array<std::int8_t, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::int8_t>(i);
    }
    return table;
}

}  // namespace

std::string base64Encode(std::string_view input) {
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= input.size(); i += 3) {
        const std::uint32_t n =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]));
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back(kAlphabet[n & 63]);
    }
    const std::size_t rem = input.size() - i;
    if (rem == 1) {
        const auto a = static_cast<std::uint32_t>(static_cast<unsigned char>(input[i]));
        out.push_back(kAlphabet[(a >> 2) & 63]);
        out.push_back(kAlphabet[(a << 4) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const auto a = static_cast<std::uint32_t>(static_cast<unsigned char>(input[i]));
        const auto b = static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]));
        out.push_back(kAlphabet[(a >> 2) & 63]);
        out.push_back(kAlphabet[((a << 4) | (b >> 4)) & 63]);
        out.push_back(kAlphabet[(b << 2) & 63]);
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view input) {
    static const std::array<std::int8_t, 256> kReverse = buildReverse();
    std::string out;
    out.reserve(input.size() / 4 * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    bool seen_padding = false;
    for (const char c : input) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;  // терпим переносы/пробелы (секреты часто переносятся построчно)
        }
        if (c == '=') {
            seen_padding = true;
            continue;
        }
        if (seen_padding) {
            return std::nullopt;  // данные после паддинга — мусор
        }
        const std::int8_t v = kReverse[static_cast<unsigned char>(c)];
        if (v < 0) {
            return std::nullopt;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace tgw::util
