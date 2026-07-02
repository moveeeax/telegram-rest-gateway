#include "ws/update_router.hpp"

#include "auth/auth_state_manager.hpp"
#include "dto/message_dto.hpp"
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
        case api::updateDeleteMessages::ID: {
            const auto& upd = static_cast<const api::updateDeleteMessages&>(update);
            Json::Value data;
            data["chat_id"] = std::to_string(upd.chat_id_);
            data["message_ids"] = idArray(upd.message_ids_);
            return ForwardableUpdate{"updateDeleteMessages", std::move(data)};
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
    frame["session_id"] = "default";
    frame["data"] = std::move(forwardable->data);

    WsSubscriberRegistry::instance().fanOut(compact(frame));
}

}  // namespace tgw::ws
