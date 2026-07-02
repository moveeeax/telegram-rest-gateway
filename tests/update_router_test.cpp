#include "ws/update_router.hpp"

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
