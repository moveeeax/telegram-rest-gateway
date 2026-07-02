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
