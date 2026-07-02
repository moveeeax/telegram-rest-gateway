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

}  // namespace

Json::Value toJson(const api::message& message) {
    Json::Value json;
    json["id"] = std::to_string(message.id_);
    json["chat_id"] = std::to_string(message.chat_id_);
    json["date"] = message.date_;
    json["is_outgoing"] = message.is_outgoing_;
    if (message.content_ != nullptr) {
        json["content"] = contentToJson(*message.content_);
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
