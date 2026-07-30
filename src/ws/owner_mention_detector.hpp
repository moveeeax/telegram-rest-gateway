#pragma once

#include <td/telegram/td_api.h>

#include <cstdint>

namespace tgw::ws {

// Причина срабатывания триггера вебхука: личка, явное упоминание владельца или reply на его
// сообщение (последнее подтверждается асинхронно после резолва автора родительского сообщения).
enum class TriggerReason { Dm, Mention, Reply };

// reply_pending: mention/dm не сработали, но есть reply_to — нужен async-резолв автора родителя
// (getMessage) на стороне вызывающего кода, прежде чем считать триггер подтверждённым.
struct DetectResult {
    bool triggered = false;      // точно триггер (dm/mention) без резолва
    bool reply_pending = false;  // требуется async getMessage(родитель)
    TriggerReason reason = TriggerReason::Dm;
};

// Чистая детекция триггера вебхука по одному входящему сообщению.
// owner_id — id владельца аккаунта (0 = ещё не известен -> сравнение с sender ложно, mention/dm
// всё равно работают, а reply_pending ставится, но подтвердить его резолвом до готовности owner_id
// вызывающий код не сможет — это приемлемо, апдейт просто не подтвердится как триггер).
inline DetectResult detect(const td::td_api::message& msg, std::int64_t owner_id,
                            bool chat_is_private, bool chat_is_broadcast) {
    namespace api = td::td_api;

    // Канал-broadcast и собственные исходящие сообщения никогда не триггерят вебхук.
    if (chat_is_broadcast || msg.is_outgoing_) {
        return {};
    }
    // Сообщение от самого владельца (например, с другого устройства) — не наш случай.
    if (msg.sender_id_ != nullptr && msg.sender_id_->get_id() == api::messageSenderUser::ID &&
        static_cast<const api::messageSenderUser&>(*msg.sender_id_).user_id_ == owner_id) {
        return {};
    }
    // Приоритет причин: явное упоминание важнее личной переписки.
    if (msg.contains_unread_mention_) {
        return {true, false, TriggerReason::Mention};
    }
    if (chat_is_private) {
        return {true, false, TriggerReason::Dm};
    }
    const bool is_reply =
        msg.reply_to_ != nullptr && msg.reply_to_->get_id() == api::messageReplyToMessage::ID;
    if (is_reply) {
        return {false, true, TriggerReason::Reply};
    }
    return {};
}

}  // namespace tgw::ws
