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

// Расширенная проекция сообщения для вебхуков mention/reply (не путать с toJson(message) —
// та используется в другом месте и не меняется): id, chat{id}, sender{id,is_bot?}, date,
// text, entities[], reply_to_message_id?, attachment{...}?. Вложение переиспользует
// contentToJson (тип/file_id/метаданные), а не дублирует его логику.
Json::Value webhookMessageToJson(const td::td_api::message& message);

}  // namespace tgw::dto
