#include "webhook/webhook_dispatcher.hpp"

#include <gtest/gtest.h>
#include <json/reader.h>
#include <json/value.h>
#include <memory>
#include <string>

using tgw::webhook::serializeEvent;
using tgw::webhook::signBody;
using tgw::webhook::WebhookEvent;

namespace {

Json::Value parse(const std::string& text) {
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    reader->parse(text.data(), text.data() + text.size(), &root, &errs);
    return root;
}

}  // namespace

// Эталон из брифа: printf '%s' 'hi' | openssl dgst -sha256 -hmac 'key'.
TEST(WebhookDispatcher, HmacSignatureKnownVector) {
    const auto sig = signBody("key", "hi");
    EXPECT_EQ(sig, "sha256=1c9dc82e5f8e5ed5a0180aad33b8204dea12fde2fb62ffb5e963035bf324a7a4");
}

// Дополнительные векторы (openssl dgst -sha256 -hmac ...): JSON-подобное тело и пустое тело —
// подпись должна оставаться строчным hex с префиксом "sha256=".
TEST(WebhookDispatcher, HmacSignatureExtraVectors) {
    EXPECT_EQ(signBody("topsecret", R"({"event_id":"s1:100:5"})"),
              "sha256=b80f5b1f03c0670dc4e843893d208747552ec00725ec4052227e0f3c8b270b69");
    EXPECT_EQ(signBody("k", ""),
              "sha256=8bb990c40a7d61cb97597a942125025be50ac8beb74436e3735b98893a7f6620");
}

// Сериализация payload'а: компактный JSON (без переводов строк), все поля события на месте,
// id — строками, message/reply_chain пробрасываются как есть.
TEST(WebhookDispatcher, SerializeEventCompactAndComplete) {
    WebhookEvent ev;
    ev.event_id = "s1:42:7";
    ev.session_id = "s1";
    ev.owner_id = "999";
    ev.trigger_reason = "mention";
    ev.received_at = 1234567890;
    ev.message = Json::Value(Json::objectValue);
    ev.message["id"] = "7";
    ev.message["text"] = "hey @owner";
    ev.reply_chain = Json::Value(Json::arrayValue);
    ev.reply_chain.append("parent");
    ev.chain_truncated = true;

    const std::string body = serializeEvent(ev);
    EXPECT_EQ(body.find('\n'), std::string::npos);  // компактно

    const Json::Value j = parse(body);
    EXPECT_EQ(j["event_id"].asString(), "s1:42:7");
    EXPECT_EQ(j["session_id"].asString(), "s1");
    EXPECT_EQ(j["owner_id"].asString(), "999");
    EXPECT_EQ(j["trigger_reason"].asString(), "mention");
    EXPECT_EQ(j["received_at"].asInt(), 1234567890);
    EXPECT_EQ(j["message"]["text"].asString(), "hey @owner");
    ASSERT_TRUE(j["reply_chain"].isArray());
    ASSERT_EQ(j["reply_chain"].size(), 1u);
    EXPECT_EQ(j["reply_chain"][0].asString(), "parent");
    EXPECT_TRUE(j["chain_truncated"].asBool());
}

// Детерминизм связки сериализация→подпись: одно и то же событие даёт стабильные тело и подпись
// (важно, т.к. подписывается ровно то, что уходит в теле POST'а).
TEST(WebhookDispatcher, SerializeThenSignIsStable) {
    WebhookEvent ev;
    ev.event_id = "s1:1:1";
    ev.session_id = "s1";
    ev.owner_id = "1";
    ev.trigger_reason = "reply";
    ev.received_at = 100;
    ev.message = Json::Value(Json::objectValue);
    ev.reply_chain = Json::Value(Json::arrayValue);

    const std::string body1 = serializeEvent(ev);
    const std::string body2 = serializeEvent(ev);
    EXPECT_EQ(body1, body2);
    EXPECT_EQ(signBody("sekret", body1), signBody("sekret", body2));
}
