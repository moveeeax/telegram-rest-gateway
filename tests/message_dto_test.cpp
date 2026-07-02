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
