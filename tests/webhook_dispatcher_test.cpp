#include "webhook/webhook_dispatcher.hpp"

#include "webhook/webhook_registry.hpp"

#include <gtest/gtest.h>
#include <json/reader.h>
#include <json/value.h>
#include <memory>
#include <optional>
#include <string>

using tgw::webhook::isPrivateHost;
using tgw::webhook::parseUrl;
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

// Стор без сети — реестр остаётся пустым (нет active-вебхуков), поэтому воркер ничего не
// шлёт: lifecycle-тест ниже НЕ бьёт реальную сеть и детерминирован под TSan.
struct EmptyStore : tgw::webhook::IWebhookStore {
    std::optional<std::string> load() override { return std::nullopt; }
    bool save(const std::string&) override { return true; }
};

WebhookEvent makeEvent(const std::string& id) {
    WebhookEvent ev;
    ev.event_id = id;
    ev.session_id = "s1";
    ev.owner_id = "1";
    ev.trigger_reason = "mention";
    ev.received_at = 1;
    ev.message = Json::Value(Json::objectValue);
    ev.reply_chain = Json::Value(Json::arrayValue);
    return ev;
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

TEST(WebhookDispatcher, IsPrivateHostBasicRanges) {
    EXPECT_TRUE(isPrivateHost(""));
    EXPECT_TRUE(isPrivateHost("localhost"));
    EXPECT_TRUE(isPrivateHost("127.0.0.1"));
    EXPECT_TRUE(isPrivateHost("10.1.2.3"));
    EXPECT_TRUE(isPrivateHost("192.168.1.1"));
    EXPECT_TRUE(isPrivateHost("172.16.0.1"));
    EXPECT_TRUE(isPrivateHost("172.31.255.255"));
    EXPECT_FALSE(isPrivateHost("172.32.0.1"));
    EXPECT_TRUE(isPrivateHost("169.254.169.254"));
    EXPECT_TRUE(isPrivateHost("0.0.0.0"));
    EXPECT_FALSE(isPrivateHost("8.8.8.8"));
    EXPECT_FALSE(isPrivateHost("example.com"));
}

TEST(WebhookDispatcher, IsPrivateHostIpv6Ranges) {
    EXPECT_TRUE(isPrivateHost("::1"));
    EXPECT_TRUE(isPrivateHost("::"));
    EXPECT_TRUE(isPrivateHost("fc00::1"));
    EXPECT_TRUE(isPrivateHost("fd12:3456::1"));
    EXPECT_TRUE(isPrivateHost("fe80::1"));
    EXPECT_FALSE(isPrivateHost("2001:4860:4860::8888"));  // публичный DNS Google — внешний
}

// Регрессия на найденный обход guard'а: IPv4-mapped/-compatible IPv6-литералы, кодирующие
// заблокированный IPv4-адрес, обязаны блокироваться так же, как голый IPv4.
TEST(WebhookDispatcher, IsPrivateHostIpv4MappedIpv6Blocked) {
    EXPECT_TRUE(isPrivateHost("::ffff:127.0.0.1"));
    EXPECT_TRUE(isPrivateHost("::ffff:169.254.169.254"));
    EXPECT_TRUE(isPrivateHost("::ffff:10.0.0.1"));
    EXPECT_TRUE(isPrivateHost("::127.0.0.1"));  // устаревшая IPv4-compatible форма
    EXPECT_FALSE(isPrivateHost("::ffff:8.8.8.8"));  // публичный IPv4 внутри mapped-нотации
}

TEST(WebhookDispatcher, ParseUrlExtractsHostAndPath) {
    const auto p1 = parseUrl("http://example.com:8080/a/b?c=1");
    EXPECT_TRUE(p1.valid);
    EXPECT_EQ(p1.base, "http://example.com:8080");
    EXPECT_EQ(p1.path, "/a/b?c=1");
    EXPECT_EQ(p1.host, "example.com");

    const auto p2 = parseUrl("https://[::ffff:127.0.0.1]:9000/x");
    EXPECT_TRUE(p2.valid);
    EXPECT_EQ(p2.host, "::ffff:127.0.0.1");

    EXPECT_FALSE(parseUrl("not-a-url").valid);
    EXPECT_FALSE(parseUrl("ftp://example.com/").valid);
}

// TGW_WEBHOOK_QUEUE_MAX=0 — валидное (не отклоняемое конфигом) значение: dispatch должен
// молча дропать КАЖДОЕ событие (queue_.size() >= queue_max_ истинно с первого раза), а не
// падать/висеть — граница из ревью, ранее не покрытая ни одним тестом.
TEST(WebhookDispatcher, QueueMaxZeroDropsEveryEvent) {
    EmptyStore store;
    tgw::webhook::WebhookRegistry reg(store);
    tgw::webhook::WebhookDispatcher d(reg, /*timeout_ms=*/200, /*queue_max=*/0,
                                      /*ssrf_guard=*/false);
    d.start();
    for (int i = 0; i < 10; ++i) {
        d.dispatch(makeEvent("q" + std::to_string(i)));  // должен дропаться, не падать/висеть
    }
    d.stop();
    SUCCEED();
}

// Полный lifecycle без сети: пустой реестр → нет active-вебхуков → воркер не шлёт запросов
// (in-flight остаётся 0), поэтому дренаж/снос loop-потока в stop() штатный и детерминированный.
// Проверяет старт/воркер/loop/барьер/дренаж/снос без heap-corruption и без зависаний под TSan.
// Заодно: dispatch до start() и после stop() — no-op (не падает, не шлёт).
TEST(WebhookDispatcher, LifecycleStartDispatchStopNoActiveHooks) {
    EmptyStore store;
    tgw::webhook::WebhookRegistry reg(store);
    tgw::webhook::WebhookDispatcher d(reg, /*timeout_ms=*/200, /*queue_max=*/128,
                                      /*ssrf_guard=*/true);

    d.dispatch(makeEvent("before-start"));  // до start() — молча игнорируется
    d.start();
    d.start();  // идемпотентно
    for (int i = 0; i < 50; ++i) {
        d.dispatch(makeEvent("e" + std::to_string(i)));
    }
    d.stop();
    d.stop();                             // идемпотентно
    d.dispatch(makeEvent("after-stop"));  // после stop() — молча игнорируется
    SUCCEED();  // цель — отсутствие креша/зависания/гонок (проверяется санитайзерами в CI)
}

// Деструктор без явного stop() при работающем диспетчере (пустой реестр) не должен виснуть/падать.
TEST(WebhookDispatcher, DestructorStopsCleanly) {
    EmptyStore store;
    tgw::webhook::WebhookRegistry reg(store);
    {
        tgw::webhook::WebhookDispatcher d(reg, 200, 128, false);
        d.start();
        d.dispatch(makeEvent("x"));
    }  // ~WebhookDispatcher → stop()
    SUCCEED();
}
