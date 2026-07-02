#include "dto/message_dto.hpp"

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>
#include <utility>

namespace api = td::td_api;

TEST(MessageDto, TextMessageWithStringIds) {
    auto msg = api::make_object<api::message>();
    msg->id_ = 123456789012345;      // > 2^32, обязан быть строкой в JSON
    msg->chat_id_ = -1001234567890;  // канал
    msg->date_ = 1700000000;
    msg->is_outgoing_ = true;
    auto text = api::make_object<api::messageText>();
    text->text_ = api::make_object<api::formattedText>();
    text->text_->text_ = "hello";
    msg->content_ = std::move(text);

    const Json::Value json = tgw::dto::toJson(*msg);
    EXPECT_EQ(json["id"].asString(), "123456789012345");
    EXPECT_EQ(json["chat_id"].asString(), "-1001234567890");
    EXPECT_TRUE(json["is_outgoing"].asBool());
    EXPECT_EQ(json["content"]["type"].asString(), "text");
    EXPECT_EQ(json["content"]["text"].asString(), "hello");
}

TEST(MessageDto, NonTextIsMarkedUnsupported) {
    auto msg = api::make_object<api::message>();
    msg->id_ = 1;
    msg->content_ = api::make_object<api::messageContactRegistered>();

    const Json::Value json = tgw::dto::toJson(*msg);
    EXPECT_EQ(json["content"]["type"].asString(), "unsupported");
    EXPECT_FALSE(json["content"]["supported"].asBool());
}

TEST(MessageDto, PhotoProjectsBestSizeAndFileId) {
    auto file = api::make_object<api::file>();
    file->id_ = 42;
    file->size_ = 1000;

    auto size_small = api::make_object<api::photoSize>();
    size_small->width_ = 90;
    size_small->height_ = 60;
    auto size_big = api::make_object<api::photoSize>();
    size_big->width_ = 1280;
    size_big->height_ = 720;
    size_big->photo_ = std::move(file);

    auto photo = api::make_object<api::photo>();
    photo->sizes_.push_back(std::move(size_small));
    photo->sizes_.push_back(std::move(size_big));

    auto content = api::make_object<api::messagePhoto>();
    content->photo_ = std::move(photo);
    content->caption_ = api::make_object<api::formattedText>();
    content->caption_->text_ = "pic!";

    auto msg = api::make_object<api::message>();
    msg->id_ = 1;
    msg->content_ = std::move(content);

    const Json::Value json = tgw::dto::toJson(*msg);
    EXPECT_EQ(json["content"]["type"].asString(), "photo");
    EXPECT_TRUE(json["content"]["supported"].asBool());
    EXPECT_EQ(json["content"]["caption"].asString(), "pic!");
    EXPECT_EQ(json["content"]["width"].asInt(), 1280);  // максимальный размер
    EXPECT_EQ(json["content"]["file_id"].asString(), "42");
    EXPECT_EQ(json["content"]["size"].asInt64(), 1000);
}

TEST(MessageDto, DocumentProjectsFileMeta) {
    auto file = api::make_object<api::file>();
    file->id_ = 7;
    file->expected_size_ = 2048;

    auto doc = api::make_object<api::document>();
    doc->file_name_ = "report.pdf";
    doc->mime_type_ = "application/pdf";
    doc->document_ = std::move(file);

    auto content = api::make_object<api::messageDocument>();
    content->document_ = std::move(doc);

    auto msg = api::make_object<api::message>();
    msg->id_ = 2;
    msg->content_ = std::move(content);

    const Json::Value json = tgw::dto::toJson(*msg);
    EXPECT_EQ(json["content"]["type"].asString(), "document");
    EXPECT_EQ(json["content"]["file_name"].asString(), "report.pdf");
    EXPECT_EQ(json["content"]["mime_type"].asString(), "application/pdf");
    EXPECT_EQ(json["content"]["file_id"].asString(), "7");
    EXPECT_EQ(json["content"]["size"].asInt64(), 2048);  // size==0 -> expected_size
}

TEST(ChatDto, Basic) {
    auto chat = api::make_object<api::chat>();
    chat->id_ = -100987654321;
    chat->title_ = "My Chat";
    chat->unread_count_ = 5;

    const Json::Value json = tgw::dto::toJson(*chat);
    EXPECT_EQ(json["id"].asString(), "-100987654321");
    EXPECT_EQ(json["title"].asString(), "My Chat");
    EXPECT_EQ(json["unread_count"].asInt(), 5);
}
