#include "ws/update_router.hpp"

#include "auth/auth_state_manager.hpp"
#include "dto/message_dto.hpp"
#include "util/metrics.hpp"
#include "ws/ws_registry.hpp"

#include <cstdint>
#include <json/writer.h>
#include <string>
#include <utility>
#include <vector>

namespace api = td::td_api;

namespace tgw::ws {
namespace {

Json::Value idArray(const std::vector<std::int64_t>& ids) {
    Json::Value arr(Json::arrayValue);
    for (const std::int64_t id : ids) {
        arr.append(std::to_string(id));
    }
    return arr;
}

std::string compact(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

}  // namespace

std::optional<ForwardableUpdate> buildForwardable(const api::Object& update) {
    switch (update.get_id()) {
        case api::updateNewMessage::ID: {
            const auto& upd = static_cast<const api::updateNewMessage&>(update);
            if (upd.message_ == nullptr) {
                return std::nullopt;
            }
            return ForwardableUpdate{"updateNewMessage", tgw::dto::toJson(*upd.message_)};
        }
        case api::updateMessageSendSucceeded::ID: {
            const auto& upd = static_cast<const api::updateMessageSendSucceeded&>(update);
            Json::Value data;
            data["old_message_id"] = std::to_string(upd.old_message_id_);
            if (upd.message_ != nullptr) {
                data["message"] = tgw::dto::toJson(*upd.message_);
            }
            return ForwardableUpdate{"updateMessageSendSucceeded", std::move(data)};
        }
        case api::updateMessageSendFailed::ID: {
            const auto& upd = static_cast<const api::updateMessageSendFailed&>(update);
            Json::Value data;
            data["old_message_id"] = std::to_string(upd.old_message_id_);
            if (upd.message_ != nullptr) {
                data["message"] = tgw::dto::toJson(*upd.message_);
            }
            return ForwardableUpdate{"updateMessageSendFailed", std::move(data)};
        }
        case api::updateNewChat::ID: {
            const auto& upd = static_cast<const api::updateNewChat&>(update);
            if (upd.chat_ == nullptr) {
                return std::nullopt;
            }
            return ForwardableUpdate{"updateNewChat", tgw::dto::toJson(*upd.chat_)};
        }
        case api::updateMessageInteractionInfo::ID: {
            const auto& upd = static_cast<const api::updateMessageInteractionInfo&>(update);
            Json::Value data;
            data["chat_id"] = std::to_string(upd.chat_id_);
            data["message_id"] = std::to_string(upd.message_id_);
            if (upd.interaction_info_ != nullptr && upd.interaction_info_->reactions_ != nullptr) {
                data["reactions"] = tgw::dto::reactionsToJson(*upd.interaction_info_->reactions_);
            } else {
                data["reactions"] = Json::Value(Json::arrayValue);
            }
            return ForwardableUpdate{"updateMessageInteractionInfo", std::move(data)};
        }
        case api::updateDeleteMessages::ID: {
            const auto& upd = static_cast<const api::updateDeleteMessages&>(update);
            Json::Value data;
            data["chat_id"] = std::to_string(upd.chat_id_);
            data["message_ids"] = idArray(upd.message_ids_);
            return ForwardableUpdate{"updateDeleteMessages", std::move(data)};
        }
        case api::updateMessageEdited::ID: {
            const auto& upd = static_cast<const api::updateMessageEdited&>(update);
            Json::Value data;
            data["chat_id"] = std::to_string(upd.chat_id_);
            data["message_id"] = std::to_string(upd.message_id_);
            data["edit_date"] = upd.edit_date_;
            return ForwardableUpdate{"updateMessageEdited", std::move(data)};
        }
        case api::updateMessageContent::ID: {
            const auto& upd = static_cast<const api::updateMessageContent&>(update);
            Json::Value data;
            data["chat_id"] = std::to_string(upd.chat_id_);
            data["message_id"] = std::to_string(upd.message_id_);
            if (upd.new_content_ != nullptr) {
                data["new_content"] = tgw::dto::contentToJson(*upd.new_content_);
            }
            return ForwardableUpdate{"updateMessageContent", std::move(data)};
        }
        case api::updateChatReadInbox::ID: {
            const auto& upd = static_cast<const api::updateChatReadInbox&>(update);
            Json::Value data;
            data["chat_id"] = std::to_string(upd.chat_id_);
            data["last_read_inbox_message_id"] = std::to_string(upd.last_read_inbox_message_id_);
            data["unread_count"] = upd.unread_count_;
            return ForwardableUpdate{"updateChatReadInbox", std::move(data)};
        }
        case api::updateUserStatus::ID: {
            const auto& upd = static_cast<const api::updateUserStatus&>(update);
            Json::Value data;
            data["user_id"] = std::to_string(upd.user_id_);
            if (upd.status_ != nullptr) {
                const Json::Value status = tgw::dto::userStatusToJson(*upd.status_);
                data["status"] = status["status"];
                if (status.isMember("was_online")) {
                    data["was_online"] = status["was_online"];
                }
            }
            return ForwardableUpdate{"updateUserStatus", std::move(data)};
        }
        case api::updateChatAction::ID: {
            const auto& upd = static_cast<const api::updateChatAction&>(update);
            Json::Value data;
            data["chat_id"] = std::to_string(upd.chat_id_);
            if (upd.sender_id_ != nullptr &&
                upd.sender_id_->get_id() == api::messageSenderUser::ID) {
                data["user_id"] = std::to_string(
                    static_cast<const api::messageSenderUser&>(*upd.sender_id_).user_id_);
            }
            // Детализацию действия сводим к двум состояниям: печатает / перестал.
            data["action"] =
                (upd.action_ != nullptr && upd.action_->get_id() == api::chatActionCancel::ID)
                    ? "cancel"
                    : "typing";
            return ForwardableUpdate{"updateChatAction", std::move(data)};
        }
        default:
            return std::nullopt;
    }
}

void UpdateRouter::onUpdate(api::object_ptr<api::Object> update) {
    if (update == nullptr) {
        return;
    }
    if (update->get_id() == api::updateAuthorizationState::ID) {
        auth_.onUpdate(std::move(update));
        return;
    }
    std::optional<ForwardableUpdate> forwardable = buildForwardable(*update);
    if (!forwardable) {
        return;  // служебный/непроецируемый апдейт — наружу не отдаём
    }
    Json::Value frame;
    frame["type"] = "update";
    frame["update_type"] = forwardable->update_type;
    frame["seq"] = static_cast<Json::UInt64>(seq_.fetch_add(1, std::memory_order_relaxed) + 1);
    frame["session_id"] = session_id_;
    frame["data"] = std::move(forwardable->data);

    tgw::metrics::Counters::instance().updates_forwarded_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
    const std::string payload = compact(frame);
    WsSubscriberRegistry::instance().fanOut(forwardable->update_type, payload);

    if (event_publisher_) {
        // Ключ: "<session_id>:<chat_id>" — префикс id аккаунта, порядок в рамках чата
        // (партиционирование Kafka по ключу). Нет chat_id — только id аккаунта.
        std::string key = session_id_;
        const Json::Value& data = frame["data"];
        if (data.isMember("chat_id") && data["chat_id"].isString()) {
            key += ":" + data["chat_id"].asString();
        } else if (data.isMember("message") && data["message"].isMember("chat_id") &&
                   data["message"]["chat_id"].isString()) {
            // updateMessageSendSucceeded/Failed: chat_id вложен в message
            key += ":" + data["message"]["chat_id"].asString();
        }
        event_publisher_(key, payload);
    }
}

}  // namespace tgw::ws
