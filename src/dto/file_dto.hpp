#pragma once

#include <td/telegram/td_api.h>

#include <json/value.h>

namespace tgw::dto {

// Проекция td_api::file (§8.3′). file_id — эфемерный (валиден в рамках сессии процесса),
// remote_unique_id — персистентный. Локальный путь наружу НЕ отдаём (стримим содержимое).
Json::Value toJson(const td::td_api::file& file);

}  // namespace tgw::dto
