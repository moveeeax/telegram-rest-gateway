#include "auth/token_store.hpp"

#include <gtest/gtest.h>

using tgw::auth::TokenStore;

TEST(TokenStore, VerifiesLoadedTokens) {
    TokenStore store;
    store.load({"tgw_alpha", "tgw_beta"});
    EXPECT_EQ(store.size(), 2u);
    EXPECT_TRUE(store.verify("tgw_alpha"));
    EXPECT_TRUE(store.verify("tgw_beta"));
    EXPECT_FALSE(store.verify("tgw_gamma"));
    EXPECT_FALSE(store.verify(""));
}

TEST(TokenStore, EmptyStoreRejectsEverything) {
    TokenStore store;
    EXPECT_TRUE(store.empty());
    EXPECT_FALSE(store.verify("anything"));
}

TEST(TokenStore, ReloadReplacesTokens) {
    TokenStore store;
    store.load({"old"});
    store.load({"new"});
    EXPECT_FALSE(store.verify("old"));
    EXPECT_TRUE(store.verify("new"));
}

TEST(TokenStore, ScopedTokenParsing) {
    tgw::auth::TokenStore store;
    store.load({"tgw_full", "tgw_ro read", "tgw_rw read,write", "tgw_typo read,wrote"});

    const auto full = store.verify("tgw_full");
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(*full, tgw::auth::kAllScopes);

    const auto ro = store.verify("tgw_ro");
    ASSERT_TRUE(ro.has_value());
    EXPECT_TRUE(tgw::auth::scopeAllows(*ro, tgw::auth::Scope::Read));
    EXPECT_FALSE(tgw::auth::scopeAllows(*ro, tgw::auth::Scope::Write));
    EXPECT_FALSE(tgw::auth::scopeAllows(*ro, tgw::auth::Scope::Admin));

    const auto rw = store.verify("tgw_rw");
    ASSERT_TRUE(rw.has_value());
    EXPECT_TRUE(tgw::auth::scopeAllows(*rw, tgw::auth::Scope::Write));
    EXPECT_FALSE(tgw::auth::scopeAllows(*rw, tgw::auth::Scope::Admin));

    // Опечатка в скоупе сужает права (fail-closed), а не расширяет.
    const auto typo = store.verify("tgw_typo");
    ASSERT_TRUE(typo.has_value());
    EXPECT_TRUE(tgw::auth::scopeAllows(*typo, tgw::auth::Scope::Read));
    EXPECT_FALSE(tgw::auth::scopeAllows(*typo, tgw::auth::Scope::Write));
}
