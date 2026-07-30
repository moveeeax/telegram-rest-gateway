#include "ws/update_router.hpp"

#include "auth/auth_state_manager.hpp"

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>
#include <utility>

namespace api = td::td_api;

TEST(UpdateRouter, ForwardsNewMessageWithStringId) {
    auto msg = api::make_object<api::message>();
    msg->id_ = 555;
    msg->chat_id_ = -100;
    auto text = api::make_object<api::messageText>();
    text->text_ = api::make_object<api::formattedText>();
    text->text_->text_ = "hi";
    msg->content_ = std::move(text);
    auto update = api::make_object<api::updateNewMessage>(std::move(msg));

    const auto forwardable = tgw::ws::buildForwardable(*update);
    ASSERT_TRUE(forwardable.has_value());
    EXPECT_EQ(forwardable->update_type, "updateNewMessage");
    EXPECT_EQ(forwardable->data["id"].asString(), "555");
}

TEST(UpdateRouter, AuthorizationStateIsNotForwarded) {
    auto update = api::make_object<api::updateAuthorizationState>(
        api::make_object<api::authorizationStateReady>());
    EXPECT_FALSE(tgw::ws::buildForwardable(*update).has_value());
}

TEST(UpdateRouter, ServiceUpdateIsNotForwarded) {
    auto update =
        api::make_object<api::updateOption>("x", api::make_object<api::optionValueBoolean>(true));
    EXPECT_FALSE(tgw::ws::buildForwardable(*update).has_value());
}

TEST(UpdateRouter, ForwardsReadInbox) {
    auto update = api::make_object<api::updateChatReadInbox>();
    update->chat_id_ = -100;
    update->last_read_inbox_message_id_ = 777;
    update->unread_count_ = 2;

    const auto forwardable = tgw::ws::buildForwardable(*update);
    ASSERT_TRUE(forwardable.has_value());
    EXPECT_EQ(forwardable->update_type, "updateChatReadInbox");
    EXPECT_EQ(forwardable->data["chat_id"].asString(), "-100");
    EXPECT_EQ(forwardable->data["last_read_inbox_message_id"].asString(), "777");
    EXPECT_EQ(forwardable->data["unread_count"].asInt(), 2);
}

TEST(UpdateRouter, ForwardsUserStatusOffline) {
    auto update = api::make_object<api::updateUserStatus>();
    update->user_id_ = 42;
    auto offline = api::make_object<api::userStatusOffline>();
    offline->was_online_ = 1234567;
    update->status_ = std::move(offline);

    const auto forwardable = tgw::ws::buildForwardable(*update);
    ASSERT_TRUE(forwardable.has_value());
    EXPECT_EQ(forwardable->update_type, "updateUserStatus");
    EXPECT_EQ(forwardable->data["user_id"].asString(), "42");
    EXPECT_EQ(forwardable->data["status"].asString(), "offline");
    EXPECT_EQ(forwardable->data["was_online"].asInt(), 1234567);
}

TEST(UpdateRouter, ForwardsEditedAndContent) {
    auto edited = api::make_object<api::updateMessageEdited>();
    edited->chat_id_ = -5;
    edited->message_id_ = 6;
    edited->edit_date_ = 100;
    auto fe = tgw::ws::buildForwardable(*edited);
    ASSERT_TRUE(fe.has_value());
    EXPECT_EQ(fe->update_type, "updateMessageEdited");

    auto content = api::make_object<api::updateMessageContent>();
    content->chat_id_ = -5;
    content->message_id_ = 6;
    auto text = api::make_object<api::messageText>();
    text->text_ = api::make_object<api::formattedText>();
    text->text_->text_ = "new";
    content->new_content_ = std::move(text);
    auto fc = tgw::ws::buildForwardable(*content);
    ASSERT_TRUE(fc.has_value());
    EXPECT_EQ(fc->update_type, "updateMessageContent");
    EXPECT_EQ(fc->data["new_content"]["text"].asString(), "new");
}

// keep-online: колбэк setOnConnectionReady вызывается на connectionStateReady и только на нём.
TEST(UpdateRouter, ConnectionReadyCallbackFiresOnlyOnReady) {
    tgw::auth::AuthStateManager auth;
    tgw::ws::UpdateRouter router(auth);
    int ready_count = 0;
    router.setOnConnectionReady([&ready_count] { ++ready_count; });

    router.onUpdate(api::make_object<api::updateConnectionState>(
        api::make_object<api::connectionStateReady>()));
    EXPECT_EQ(ready_count, 1);  // Ready → колбэк вызван

    router.onUpdate(api::make_object<api::updateConnectionState>(
        api::make_object<api::connectionStateConnecting>()));
    EXPECT_EQ(ready_count, 1);  // иное состояние → колбэк НЕ вызван

    router.onUpdate(api::make_object<api::updateConnectionState>(
        api::make_object<api::connectionStateReady>()));
    EXPECT_EQ(ready_count, 2);  // повторный Ready (реконнект) → снова вызван
}

// Без зарегистрированного колбэка updateConnectionState не падает (keep-online выключен).
TEST(UpdateRouter, ConnectionReadyWithoutCallbackDoesNotCrash) {
    tgw::auth::AuthStateManager auth;
    tgw::ws::UpdateRouter router(auth);
    router.onUpdate(api::make_object<api::updateConnectionState>(
        api::make_object<api::connectionStateReady>()));
    SUCCEED();
}

TEST(UpdateRouter, ForwardsChatActionTypingAndCancel) {
    auto typing = api::make_object<api::updateChatAction>();
    typing->chat_id_ = -9;
    typing->sender_id_ = api::make_object<api::messageSenderUser>(11);
    typing->action_ = api::make_object<api::chatActionTyping>();
    auto ft = tgw::ws::buildForwardable(*typing);
    ASSERT_TRUE(ft.has_value());
    EXPECT_EQ(ft->data["action"].asString(), "typing");
    EXPECT_EQ(ft->data["user_id"].asString(), "11");

    auto cancel = api::make_object<api::updateChatAction>();
    cancel->chat_id_ = -9;
    cancel->action_ = api::make_object<api::chatActionCancel>();
    auto fx = tgw::ws::buildForwardable(*cancel);
    ASSERT_TRUE(fx.has_value());
    EXPECT_EQ(fx->data["action"].asString(), "cancel");
}
