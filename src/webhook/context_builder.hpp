#pragma once

#include "bridge/td_bridge.hpp"
#include "ws/owner_mention_detector.hpp"

#include <td/telegram/td_api.h>

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>

namespace tgw::webhook {

// Готовое к рассылке событие вебхука: плоская проекция входящего сообщения плюс поднятая
// reply-цепочка. Все id — строками (§8.2.1). received_at приходит снаружи (в проде — из
// msg.date_ / часов моста, в тестах — фиксом), НЕ Date::now() внутри чистой логики.
struct WebhookEvent {
    std::string event_id;       // session_id:chat_id:message_id
    std::string session_id;     // сессия-источник (из вызывающего)
    std::string owner_id;       // владелец аккаунта (из вызывающего)
    std::string trigger_reason;  // "mention" | "dm" | "reply"
    std::int32_t received_at = 0;
    Json::Value message;      // webhookMessageToJson(msg)
    Json::Value reply_chain;  // массив родителей в порядке родитель -> корень
    bool chain_truncated = false;
};

// Async-построение события. Поднимает reply-цепочку через getMessage (по одному звену на co_await),
// пока есть родитель, до chain_limit звеньев. Возвращает nullopt, если триггер не подтвердился:
// det.reply_pending, но автор первого родителя != owner (либо reply_pending без родителя вовсе).
// client_id/owner_id/session_id/received_at — из вызывающего кода.
drogon::Task<std::optional<WebhookEvent>> buildEvent(tgw::bridge::TdBridge& bridge,
                                                     std::int32_t client_id,
                                                     const td::td_api::message& msg,
                                                     tgw::ws::DetectResult det,
                                                     std::int64_t owner_id, std::string session_id,
                                                     std::int32_t received_at,
                                                     int chain_limit = 20);

}  // namespace tgw::webhook
