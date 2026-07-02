#include "dto/message_dto.hpp"

#include <string>

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
