#include "webhook/context_builder.hpp"

#include "dto/message_dto.hpp"

#include <string>
#include <utility>

namespace api = td::td_api;

namespace tgw::webhook {
namespace {

// message_id родителя, если сообщение — reply на другое сообщение (messageReplyToMessage).
// Иные варианты reply_to_ (напр. reply на историю) родителем-сообщением не считаем.
std::optional<std::int64_t> replyParentId(const api::message& msg) {
    if (msg.reply_to_ != nullptr && msg.reply_to_->get_id() == api::messageReplyToMessage::ID) {
        return static_cast<const api::messageReplyToMessage&>(*msg.reply_to_).message_id_;
    }
    return std::nullopt;
}

// Автор сообщения — владелец аккаунта? Тот же паттерн, что в owner_mention_detector (Task 2):
// sender_id_ должен быть messageSenderUser с user_id_ == owner_id.
bool senderIsOwner(const api::message& msg, std::int64_t owner_id) {
    return msg.sender_id_ != nullptr && msg.sender_id_->get_id() == api::messageSenderUser::ID &&
           static_cast<const api::messageSenderUser&>(*msg.sender_id_).user_id_ == owner_id;
}

}  // namespace

drogon::Task<std::optional<WebhookEvent>> buildEvent(tgw::bridge::TdBridge& bridge,
                                                     std::int32_t client_id,
                                                     const api::message& msg, tgw::ws::DetectResult det,
                                                     std::int64_t owner_id, std::string session_id,
                                                     std::int32_t received_at, int chain_limit) {
    WebhookEvent ev;
    ev.session_id = session_id;
    ev.owner_id = std::to_string(owner_id);
    ev.received_at = received_at;
    ev.message = tgw::dto::webhookMessageToJson(msg);
    ev.event_id =
        session_id + ":" + std::to_string(msg.chat_id_) + ":" + std::to_string(msg.id_);
    ev.reply_chain = Json::Value(Json::arrayValue);

    // Снимаем всё нужное из msg ДО первой точки suspend: дальше в цепочке co_await ссылка на msg
    // не используется (chat_id зафиксирован локально, родители приходят из ответов getMessage).
    // Это делает корутину устойчивой к тому, что вызывающий не удержит msg живым за первым co_await.
    const std::int64_t chat_id = msg.chat_id_;
    const std::optional<std::int64_t> parent_id = replyParentId(msg);

    bool truncated = false;
    if (parent_id) {
        std::int64_t cur = *parent_id;
        int hops = 0;
        while (cur != 0 && hops < chain_limit) {
            auto obj = co_await bridge.invoke(
                client_id, api::make_object<api::getMessage>(chat_id, cur));
            if (obj == nullptr || obj->get_id() == api::error::ID) {
                truncated = true;  // недоступное звено обрывает цепочку, событие остаётся частичным
                break;
            }
            const auto& parent = static_cast<const api::message&>(*obj);
            // Подтверждение reply-триггера — на первом (ближайшем) родителе: автор должен быть owner.
            if (hops == 0 && det.reply_pending) {
                if (!senderIsOwner(parent, owner_id)) {
                    co_return std::nullopt;
                }
                ev.trigger_reason = "reply";
            }
            ev.reply_chain.append(tgw::dto::webhookMessageToJson(parent));
            cur = replyParentId(parent).value_or(0);
            ++hops;
        }
        // Остался нераскрытый родитель (cur != 0) при упоре в лимит — помечаем усечение.
        if (cur != 0) {
            truncated = truncated || (hops >= chain_limit);
        }
    } else if (det.reply_pending) {
        // reply_pending без родителя невозможно подтвердить — это не триггер.
        co_return std::nullopt;
    }

    ev.chain_truncated = truncated;
    if (ev.trigger_reason.empty()) {
        ev.trigger_reason = det.reason == tgw::ws::TriggerReason::Mention  ? "mention"
                            : det.reason == tgw::ws::TriggerReason::Dm     ? "dm"
                                                                           : "reply";
    }
    co_return std::optional<WebhookEvent>(std::move(ev));
}

}  // namespace tgw::webhook
