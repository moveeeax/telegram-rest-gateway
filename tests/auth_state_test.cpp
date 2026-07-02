#include <gtest/gtest.h>
#include <td/telegram/td_api.h>

#include <utility>

#include "auth/auth_state.hpp"
#include "auth/auth_state_manager.hpp"

namespace api = td::td_api;
using tgw::auth::AuthState;
using tgw::auth::AuthStateManager;

namespace {

api::object_ptr<api::Object> authUpdate(api::object_ptr<api::AuthorizationState> state) {
    return api::make_object<api::updateAuthorizationState>(std::move(state));
}

}  // namespace

TEST(AuthStateManager, MapsReadyAndBumpsGeneration) {
    AuthStateManager manager;
    EXPECT_EQ(manager.current(), AuthState::Unknown);
    EXPECT_EQ(manager.generation(), 0u);

    manager.onUpdate(authUpdate(api::make_object<api::authorizationStateReady>()));

    EXPECT_EQ(manager.current(), AuthState::Ready);
    EXPECT_EQ(manager.generation(), 1u);
}

TEST(AuthStateManager, MapsWaitPhoneNumber) {
    AuthStateManager manager;
    manager.onUpdate(authUpdate(api::make_object<api::authorizationStateWaitPhoneNumber>()));
    EXPECT_EQ(manager.current(), AuthState::WaitPhoneNumber);
}

TEST(AuthStateManager, NonAuthUpdateOnlyCounts) {
    AuthStateManager manager;
    manager.onUpdate(api::make_object<api::updateOption>(
        "version", api::make_object<api::optionValueString>("1.8.0")));
    EXPECT_EQ(manager.current(), AuthState::Unknown);
    EXPECT_EQ(manager.generation(), 0u);
    EXPECT_EQ(manager.updateCount(), 1u);
}

TEST(AuthState, ToStringIsStable) {
    EXPECT_EQ(tgw::auth::toString(AuthState::WaitCode), "wait_code");
    EXPECT_EQ(tgw::auth::toString(AuthState::Ready), "ready");
    EXPECT_EQ(tgw::auth::toString(AuthState::Unsupported), "unsupported_state");
}
