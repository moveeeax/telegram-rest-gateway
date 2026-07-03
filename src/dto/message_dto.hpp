#pragma once

#include <td/telegram/td_api.h>

#include <json/value.h>

namespace tgw::dto {

// Тонкие проекции td_api -> JSON (§8.2.2, §8.2.7). Все 64-битные id — СТРОКАМИ (§8.2.1);
// не-текстовый контент отдаётся с дискриминатором type и supported:false (не выкидывается).
Json::Value toJson(const td::td_api::message& message);
Json::Value toJson(const td::td_api::chat& chat);
Json::Value toJson(const td::td_api::user& user);
Json::Value contentToJson(const td::td_api::MessageContent& content);
Json::Value userStatusToJson(const td::td_api::UserStatus& status);
Json::Value toJson(const td::td_api::chatMember& member);

// Реакции сообщения → массив {reaction:{type,emoji}, total_count, is_chosen}.
Json::Value reactionsToJson(const td::td_api::messageReactions& reactions);

}  // namespace tgw::dto
