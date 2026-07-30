#include "webhook/webhook_registry.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace {

// Фейковый стор без сети: держит "сохранённый" JSON в поле, save может имитировать сбой.
struct FakeStore : tgw::webhook::IWebhookStore {
    std::optional<std::string> data;
    bool fail_save = false;

    std::optional<std::string> load() override { return data; }
    bool save(const std::string& j) override {
        if (fail_save) {
            return false;
        }
        data = j;
        return true;
    }
};

}  // namespace

TEST(WebhookRegistry, AddListPersist) {
    FakeStore s;
    tgw::webhook::WebhookRegistry r(s);
    const auto id = r.add("https://h/1", "sekret", true);
    EXPECT_FALSE(id.empty());
    ASSERT_TRUE(s.data.has_value());  // персистнули
    const auto l = r.list();
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].url, "https://h/1");
}

TEST(WebhookRegistry, LoadFromStoreRoundTrip) {
    FakeStore s;
    {
        tgw::webhook::WebhookRegistry r(s);
        r.add("https://h/1", "x", true);
    }
    tgw::webhook::WebhookRegistry r2(s);
    r2.loadFromStore();
    ASSERT_EQ(r2.list().size(), 1u);
    EXPECT_EQ(r2.activeSnapshot().size(), 1u);
}

TEST(WebhookRegistry, RemoveAndInactive) {
    FakeStore s;
    tgw::webhook::WebhookRegistry r(s);
    const auto id = r.add("https://h/1", "x", false);
    EXPECT_EQ(r.activeSnapshot().size(), 0u);  // inactive не в snapshot
    EXPECT_TRUE(r.remove(id));
    EXPECT_EQ(r.list().size(), 0u);
}

TEST(WebhookRegistry, EmptyStoreLoadsEmpty) {
    FakeStore s;
    tgw::webhook::WebhookRegistry r(s);
    r.loadFromStore();
    EXPECT_EQ(r.list().size(), 0u);
}
