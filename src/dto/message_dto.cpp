#include "dto/message_dto.hpp"

#include <string>
#include <vector>

namespace api = td::td_api;

namespace tgw::dto {
namespace {

// Ссылка на файл TDLib: file_id пригоден для GET /v1/files/{id} (эфемерен в рамках сессии).
void putFileRef(Json::Value& json, const api::file* file) {
    if (file == nullptr) {
        return;
    }
    json["file_id"] = std::to_string(file->id_);
    json["size"] = static_cast<Json::Int64>(file->size_ != 0 ? file->size_ : file->expected_size_);
}

std::string captionText(const api::formattedText* caption) {
    return (caption != nullptr) ? caption->text_ : "";
}

}  // namespace

Json::Value contentToJson(const api::MessageContent& content) {
    Json::Value json;
    json["supported"] = true;
    switch (content.get_id()) {
        case api::messageText::ID: {
            const auto& c = static_cast<const api::messageText&>(content);
            json["type"] = "text";
            json["text"] = (c.text_ != nullptr) ? c.text_->text_ : "";
            break;
        }
        case api::messagePhoto::ID: {
            const auto& c = static_cast<const api::messagePhoto&>(content);
            json["type"] = "photo";
            json["caption"] = captionText(c.caption_.get());
            // Берём максимальный по ширине размер (порядок sizes не гарантирован для
            // только что отправленных своих фото).
            if (c.photo_ != nullptr && !c.photo_->sizes_.empty()) {
                const api::photoSize* best = nullptr;
                for (const auto& size : c.photo_->sizes_) {
                    if (size != nullptr && (best == nullptr || size->width_ > best->width_)) {
                        best = size.get();
                    }
                }
                if (best != nullptr) {
                    json["width"] = best->width_;
                    json["height"] = best->height_;
                    putFileRef(json, best->photo_.get());
                }
            }
            break;
        }
        case api::messageVideo::ID: {
            const auto& c = static_cast<const api::messageVideo&>(content);
            json["type"] = "video";
            json["caption"] = captionText(c.caption_.get());
            if (c.video_ != nullptr) {
                json["duration"] = c.video_->duration_;
                json["width"] = c.video_->width_;
                json["height"] = c.video_->height_;
                json["file_name"] = c.video_->file_name_;
                json["mime_type"] = c.video_->mime_type_;
                putFileRef(json, c.video_->video_.get());
            }
            break;
        }
        case api::messageVoiceNote::ID: {
            const auto& c = static_cast<const api::messageVoiceNote&>(content);
            json["type"] = "voice";
            json["caption"] = captionText(c.caption_.get());
            if (c.voice_note_ != nullptr) {
                json["duration"] = c.voice_note_->duration_;
                json["mime_type"] = c.voice_note_->mime_type_;
                putFileRef(json, c.voice_note_->voice_.get());
            }
            break;
        }
        case api::messageAudio::ID: {
            const auto& c = static_cast<const api::messageAudio&>(content);
            json["type"] = "audio";
            json["caption"] = captionText(c.caption_.get());
            if (c.audio_ != nullptr) {
                json["duration"] = c.audio_->duration_;
                json["title"] = c.audio_->title_;
                json["performer"] = c.audio_->performer_;
                json["file_name"] = c.audio_->file_name_;
                json["mime_type"] = c.audio_->mime_type_;
                putFileRef(json, c.audio_->audio_.get());
            }
            break;
        }
        case api::messageDocument::ID: {
            const auto& c = static_cast<const api::messageDocument&>(content);
            json["type"] = "document";
            json["caption"] = captionText(c.caption_.get());
            if (c.document_ != nullptr) {
                json["file_name"] = c.document_->file_name_;
                json["mime_type"] = c.document_->mime_type_;
                putFileRef(json, c.document_->document_.get());
            }
            break;
        }
        case api::messageSticker::ID: {
            const auto& c = static_cast<const api::messageSticker&>(content);
            json["type"] = "sticker";
            if (c.sticker_ != nullptr) {
                json["emoji"] = c.sticker_->emoji_;
                json["width"] = c.sticker_->width_;
                json["height"] = c.sticker_->height_;
                putFileRef(json, c.sticker_->sticker_.get());
            }
            break;
        }
        case api::messageAnimation::ID: {
            const auto& c = static_cast<const api::messageAnimation&>(content);
            json["type"] = "animation";
            json["caption"] = captionText(c.caption_.get());
            if (c.animation_ != nullptr) {
                json["duration"] = c.animation_->duration_;
                json["width"] = c.animation_->width_;
                json["height"] = c.animation_->height_;
                json["mime_type"] = c.animation_->mime_type_;
                putFileRef(json, c.animation_->animation_.get());
            }
            break;
        }
        case api::messageVideoNote::ID: {
            const auto& c = static_cast<const api::messageVideoNote&>(content);
            json["type"] = "video_note";
            if (c.video_note_ != nullptr) {
                json["duration"] = c.video_note_->duration_;
                putFileRef(json, c.video_note_->video_.get());
            }
            break;
        }
        default:
            // Прочий контент (опросы, локации, сервисные…) — метаинфо без проекции.
            json["type"] = "unsupported";
            json["supported"] = false;
    }
    return json;
}

namespace {

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
    if (chat.type_ != nullptr) {
        switch (chat.type_->get_id()) {
            case api::chatTypePrivate::ID:
                json["type"] = "private";
                json["user_id"] =
                    std::to_string(static_cast<const api::chatTypePrivate&>(*chat.type_).user_id_);
                break;
            case api::chatTypeBasicGroup::ID:
                json["type"] = "group";
                break;
            case api::chatTypeSupergroup::ID:
                json["type"] = static_cast<const api::chatTypeSupergroup&>(*chat.type_).is_channel_
                                   ? "channel"
                                   : "supergroup";
                break;
            case api::chatTypeSecret::ID:
                json["type"] = "secret";
                break;
            default:
                json["type"] = "other";
        }
    }
    return json;
}

Json::Value toJson(const api::user& user) {
    Json::Value json;
    json["id"] = std::to_string(user.id_);
    json["first_name"] = user.first_name_;
    json["last_name"] = user.last_name_;
    if (user.usernames_ != nullptr && !user.usernames_->active_usernames_.empty()) {
        json["username"] = user.usernames_->active_usernames_.front();
    }
    json["phone_number"] = user.phone_number_;
    json["is_contact"] = user.is_contact_;
    json["is_premium"] = user.is_premium_;
    if (user.status_ != nullptr) {
        const Json::Value status = userStatusToJson(*user.status_);
        json["status"] = status["status"];
        if (status.isMember("was_online")) {
            json["was_online"] = status["was_online"];
        }
    }
    return json;
}

Json::Value userStatusToJson(const api::UserStatus& status) {
    Json::Value json;
    switch (status.get_id()) {
        case api::userStatusOnline::ID:
            json["status"] = "online";
            break;
        case api::userStatusOffline::ID:
            json["status"] = "offline";
            json["was_online"] = static_cast<const api::userStatusOffline&>(status).was_online_;
            break;
        case api::userStatusRecently::ID:
            json["status"] = "recently";
            break;
        case api::userStatusLastWeek::ID:
            json["status"] = "last_week";
            break;
        case api::userStatusLastMonth::ID:
            json["status"] = "last_month";
            break;
        default:
            json["status"] = "unknown";
    }
    return json;
}

Json::Value toJson(const api::chatMember& member) {
    Json::Value json;
    if (member.member_id_ != nullptr) {
        if (member.member_id_->get_id() == api::messageSenderUser::ID) {
            json["user_id"] = std::to_string(
                static_cast<const api::messageSenderUser&>(*member.member_id_).user_id_);
        } else if (member.member_id_->get_id() == api::messageSenderChat::ID) {
            json["chat_id"] = std::to_string(
                static_cast<const api::messageSenderChat&>(*member.member_id_).chat_id_);
        }
    }
    json["joined_date"] = member.joined_chat_date_;
    if (member.status_ != nullptr) {
        switch (member.status_->get_id()) {
            case api::chatMemberStatusCreator::ID:
                json["status"] = "creator";
                break;
            case api::chatMemberStatusAdministrator::ID:
                json["status"] = "administrator";
                break;
            case api::chatMemberStatusMember::ID:
                json["status"] = "member";
                break;
            case api::chatMemberStatusRestricted::ID:
                json["status"] = "restricted";
                break;
            case api::chatMemberStatusLeft::ID:
                json["status"] = "left";
                break;
            case api::chatMemberStatusBanned::ID:
                json["status"] = "banned";
                break;
            default:
                json["status"] = "unknown";
        }
    }
    return json;
}

namespace {

// sender{id, is_bot?} вебхук-проекции. MessageSender сам по себе не несёт признака "бот" —
// это свойство полного api::user, который здесь недоступен (только id отправителя), поэтому
// is_bot попросту не проставляется (см. бриф task-3: "если доступно, иначе опустить").
Json::Value webhookSenderToJson(const api::MessageSender& sender) {
    Json::Value json;
    switch (sender.get_id()) {
        case api::messageSenderUser::ID:
            json["id"] =
                std::to_string(static_cast<const api::messageSenderUser&>(sender).user_id_);
            break;
        case api::messageSenderChat::ID:
            json["id"] =
                std::to_string(static_cast<const api::messageSenderChat&>(sender).chat_id_);
            break;
        default:
            break;
    }
    return json;
}

// Короткий дискриминатор типа entity. mention/mention_name — обязательные по брифу, остальные —
// best-effort (расширяют покрытие форматирования без риска сломать обязательные кейсы).
Json::Value webhookEntityTypeToJson(const api::TextEntityType& type) {
    Json::Value json;
    switch (type.get_id()) {
        case api::textEntityTypeMention::ID:
            json["type"] = "mention";
            break;
        case api::textEntityTypeMentionName::ID:
            json["type"] = "mention_name";
            json["user_id"] = std::to_string(
                static_cast<const api::textEntityTypeMentionName&>(type).user_id_);
            break;
        case api::textEntityTypeHashtag::ID:
            json["type"] = "hashtag";
            break;
        case api::textEntityTypeCashtag::ID:
            json["type"] = "cashtag";
            break;
        case api::textEntityTypeBotCommand::ID:
            json["type"] = "bot_command";
            break;
        case api::textEntityTypeUrl::ID:
            json["type"] = "url";
            break;
        case api::textEntityTypeTextUrl::ID:
            json["type"] = "text_url";
            json["url"] = static_cast<const api::textEntityTypeTextUrl&>(type).url_;
            break;
        case api::textEntityTypeEmailAddress::ID:
            json["type"] = "email_address";
            break;
        case api::textEntityTypePhoneNumber::ID:
            json["type"] = "phone_number";
            break;
        case api::textEntityTypeBold::ID:
            json["type"] = "bold";
            break;
        case api::textEntityTypeItalic::ID:
            json["type"] = "italic";
            break;
        case api::textEntityTypeUnderline::ID:
            json["type"] = "underline";
            break;
        case api::textEntityTypeStrikethrough::ID:
            json["type"] = "strikethrough";
            break;
        case api::textEntityTypeSpoiler::ID:
            json["type"] = "spoiler";
            break;
        case api::textEntityTypeCode::ID:
            json["type"] = "code";
            break;
        case api::textEntityTypePre::ID:
            json["type"] = "pre";
            break;
        case api::textEntityTypePreCode::ID:
            json["type"] = "pre_code";
            break;
        case api::textEntityTypeBlockQuote::ID:
            json["type"] = "block_quote";
            break;
        case api::textEntityTypeCustomEmoji::ID:
            json["type"] = "custom_emoji";
            break;
        default:
            json["type"] = "other";
    }
    return json;
}

Json::Value webhookEntitiesToJson(const std::vector<api::object_ptr<api::textEntity>>& entities) {
    Json::Value arr(Json::arrayValue);
    for (const auto& entity : entities) {
        if (entity == nullptr || entity->type_ == nullptr) {
            continue;
        }
        Json::Value json = webhookEntityTypeToJson(*entity->type_);
        json["offset"] = entity->offset_;
        json["length"] = entity->length_;
        arr.append(json);
    }
    return arr;
}

// Источник text/entities для проекции: у messageText — это text_, у контента с подписью
// (фото/видео/голос/аудио/документ/анимация) — caption_. Остальной контент текста не несёт.
const api::formattedText* webhookExtractFormattedText(const api::MessageContent& content) {
    switch (content.get_id()) {
        case api::messageText::ID:
            return static_cast<const api::messageText&>(content).text_.get();
        case api::messagePhoto::ID:
            return static_cast<const api::messagePhoto&>(content).caption_.get();
        case api::messageVideo::ID:
            return static_cast<const api::messageVideo&>(content).caption_.get();
        case api::messageVoiceNote::ID:
            return static_cast<const api::messageVoiceNote&>(content).caption_.get();
        case api::messageAudio::ID:
            return static_cast<const api::messageAudio&>(content).caption_.get();
        case api::messageDocument::ID:
            return static_cast<const api::messageDocument&>(content).caption_.get();
        case api::messageAnimation::ID:
            return static_cast<const api::messageAnimation&>(content).caption_.get();
        default:
            return nullptr;
    }
}

}  // namespace

Json::Value webhookMessageToJson(const api::message& message) {
    Json::Value json;
    json["id"] = std::to_string(message.id_);
    json["chat"]["id"] = std::to_string(message.chat_id_);
    json["date"] = message.date_;
    if (message.sender_id_ != nullptr) {
        json["sender"] = webhookSenderToJson(*message.sender_id_);
    }

    json["text"] = "";
    json["entities"] = Json::Value(Json::arrayValue);
    if (message.content_ != nullptr) {
        const api::formattedText* formatted = webhookExtractFormattedText(*message.content_);
        if (formatted != nullptr) {
            json["text"] = formatted->text_;
            json["entities"] = webhookEntitiesToJson(formatted->entities_);
        }

        // Вложение: переиспользуем существующую contentToJson (тип/file_id/метаданные) —
        // не дублируем логику извлечения file_id/размеров/длительности. Для чистого текста
        // вложения нет (type == "text").
        const Json::Value content_json = contentToJson(*message.content_);
        if (content_json["type"].asString() != "text") {
            json["attachment"] = content_json;
        }
    }

    if (message.reply_to_ != nullptr &&
        message.reply_to_->get_id() == api::messageReplyToMessage::ID) {
        json["reply_to_message_id"] = std::to_string(
            static_cast<const api::messageReplyToMessage&>(*message.reply_to_).message_id_);
    }

    return json;
}

}  // namespace tgw::dto
