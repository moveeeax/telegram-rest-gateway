#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace tgw::util {

// Стандартный base64 (RFC 4648, с '='-паддингом). Для сериализации session string
// (td.binlog <-> env). Decode терпим к переводам строк/пробелам; при мусоре — nullopt.
std::string base64Encode(std::string_view input);
std::optional<std::string> base64Decode(std::string_view input);

}  // namespace tgw::util
