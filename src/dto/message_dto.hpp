#pragma once

#include <td/telegram/td_api.h>

#include <json/value.h>

namespace tgw::dto {

// Тонкие проекции td_api -> JSON (§8.2.2, §8.2.7). Все 64-битные id — СТРОКАМИ (§8.2.1);
// не-текстовый контент отдаётся с дискриминатором type и supported:false (не выкидывается).
Json::Value toJson(const td::td_api::message& message);
Json::Value toJson(const td::td_api::chat& chat);

}  // namespace tgw::dto
