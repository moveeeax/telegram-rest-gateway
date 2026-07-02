#include "dto/message_dto.hpp"

#include <string>

namespace api = td::td_api;

namespace tgw::dto {
namespace {

Json::Value contentToJson(const api::MessageContent& content) {
    Json::Value json;
    if (content.get_id() == api::messageText::ID) {
        const auto& text = static_cast<const api::messageText&>(content);
        json["type"] = "text";
        json["supported"] = true;
        json["text"] = (text.text_ != nullptr) ? text.text_->text_ : "";
    } else {
        // Не-текстовый контент (§8.2.7): метаинфо есть, но полная проекция — post-MVP.
        json["type"] = "unsupported";
        json["supported"] = false;
    }
    return json;
}

Json::Value reactionTypeToJson(const api::ReactionType& type) {
    Json::Value json;
    switch (type.get_id()) {
        case api::reactionTypeEmoji::ID:
            json["type"] = "emoji";
            json["emoji"] = static_cast<const api::reactionTypeEmoji&>(type).emoji_;
            break;
        case api::reactionTypeCustomEmoji::ID:
            json["type"] = "custom_emoji";
            json["custom_emoji_id"] = std::to_string(
                static_cast<const api::reactionTypeCustomEmoji&>(type).custom_emoji_id_);
            break;
        default:
            json["type"] = "other";
    }
    return json;
}

}  // namespace

Json::Value reactionsToJson(const api::messageReactions& reactions) {
    Json::Value arr(Json::arrayValue);
    for (const auto& reaction : reactions.reactions_) {
        if (reaction == nullptr) {
            continue;
        }
        Json::Value json;
        if (reaction->type_ != nullptr) {
            json["reaction"] = reactionTypeToJson(*reaction->type_);
        }
        json["total_count"] = reaction->total_count_;
        json["is_chosen"] = reaction->is_chosen_;
        arr.append(json);
    }
    return arr;
}

Json::Value toJson(const api::message& message) {
    Json::Value json;
    json["id"] = std::to_string(message.id_);
    json["chat_id"] = std::to_string(message.chat_id_);
    json["date"] = message.date_;
    json["is_outgoing"] = message.is_outgoing_;
    if (message.content_ != nullptr) {
        json["content"] = contentToJson(*message.content_);
    }
    if (message.interaction_info_ != nullptr && message.interaction_info_->reactions_ != nullptr) {
        json["reactions"] = reactionsToJson(*message.interaction_info_->reactions_);
    }
    return json;
}

Json::Value toJson(const api::chat& chat) {
    Json::Value json;
    json["id"] = std::to_string(chat.id_);
    json["title"] = chat.title_;
    json["unread_count"] = chat.unread_count_;
    return json;
}

}  // namespace tgw::dto
