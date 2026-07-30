#include "ws/owner_mention_detector.hpp"

#include <td/telegram/td_api.h>

#include <cstdint>
#include <gtest/gtest.h>

namespace api = td::td_api;
using tgw::ws::detect;
using tgw::ws::TriggerReason;

namespace {

api::object_ptr<api::message> mkMsg(bool outgoing, std::int64_t sender_uid, bool mention,
                                     bool has_reply) {
    auto m = api::make_object<api::message>();
    m->is_outgoing_ = outgoing;
    m->contains_unread_mention_ = mention;
    m->sender_id_ = api::make_object<api::messageSenderUser>(sender_uid);
    if (has_reply) {
        m->reply_to_ = api::make_object<api::messageReplyToMessage>();
    }
    return m;
}

}  // namespace

TEST(OwnerMentionDetector, DmAnyIncomingTriggers) {
    auto m = mkMsg(/*outgoing=*/false, /*sender=*/555, /*mention=*/false, /*has_reply=*/false);
    auto r = detect(*m, /*owner_id=*/111, /*chat_is_private=*/true, /*chat_is_broadcast=*/false);
    EXPECT_TRUE(r.triggered);
    EXPECT_EQ(r.reason, TriggerReason::Dm);
}

TEST(OwnerMentionDetector, OutgoingNeverTriggers) {
    auto m = mkMsg(/*outgoing=*/true, /*sender=*/111, /*mention=*/true, /*has_reply=*/true);
    auto r = detect(*m, /*owner_id=*/111, /*chat_is_private=*/true, /*chat_is_broadcast=*/false);
    EXPECT_FALSE(r.triggered);
    EXPECT_FALSE(r.reply_pending);
}

TEST(OwnerMentionDetector, MentionFlagTriggersInGroup) {
    auto m = mkMsg(/*outgoing=*/false, /*sender=*/555, /*mention=*/true, /*has_reply=*/false);
    auto r = detect(*m, /*owner_id=*/111, /*chat_is_private=*/false, /*chat_is_broadcast=*/false);
    EXPECT_TRUE(r.triggered);
    EXPECT_EQ(r.reason, TriggerReason::Mention);
}

TEST(OwnerMentionDetector, ReplyInGroupIsPending) {
    auto m = mkMsg(/*outgoing=*/false, /*sender=*/555, /*mention=*/false, /*has_reply=*/true);
    auto r = detect(*m, /*owner_id=*/111, /*chat_is_private=*/false, /*chat_is_broadcast=*/false);
    EXPECT_FALSE(r.triggered);
    EXPECT_TRUE(r.reply_pending);
    EXPECT_EQ(r.reason, TriggerReason::Reply);
}

TEST(OwnerMentionDetector, BroadcastChannelNeverTriggers) {
    auto m = mkMsg(/*outgoing=*/false, /*sender=*/555, /*mention=*/true, /*has_reply=*/true);
    auto r = detect(*m, /*owner_id=*/111, /*chat_is_private=*/false, /*chat_is_broadcast=*/true);
    EXPECT_FALSE(r.triggered);
    EXPECT_FALSE(r.reply_pending);
}

TEST(OwnerMentionDetector, OwnSenderNotTrigger) {
    auto m = mkMsg(/*outgoing=*/false, /*sender==owner=*/111, /*mention=*/false,
                    /*has_reply=*/false);
    auto r = detect(*m, /*owner_id=*/111, /*chat_is_private=*/true, /*chat_is_broadcast=*/false);
    EXPECT_FALSE(r.triggered);
}

// Ботов по sender_id не фильтруем — только совпадение с owner_id.
TEST(OwnerMentionDetector, BotSenderStillTriggersDm) {
    auto m = mkMsg(/*outgoing=*/false, /*sender=*/777, /*mention=*/false, /*has_reply=*/false);
    auto r = detect(*m, /*owner_id=*/111, /*chat_is_private=*/true, /*chat_is_broadcast=*/false);
    EXPECT_TRUE(r.triggered);
}

// Reply в личке классифицируется как Reply (приоритет reply > dm), а НЕ Dm, и остаётся pending —
// автор родителя дорезолвивается на стороне вызывающего.
TEST(OwnerMentionDetector, ReplyInPrivateIsReplyNotDm) {
    auto m = mkMsg(/*outgoing=*/false, /*sender=*/555, /*mention=*/false, /*has_reply=*/true);
    auto r = detect(*m, /*owner_id=*/111, /*chat_is_private=*/true, /*chat_is_broadcast=*/false);
    EXPECT_FALSE(r.triggered);
    EXPECT_TRUE(r.reply_pending);
    EXPECT_EQ(r.reason, TriggerReason::Reply);
}
