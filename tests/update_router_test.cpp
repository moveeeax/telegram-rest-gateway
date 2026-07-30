#include "ws/update_router.hpp"

#include "auth/auth_state_manager.hpp"
#include "bridge/message_send_tracker.hpp"

#include <td/telegram/td_api.h>

#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoop.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace api = td::td_api;

using namespace std::chrono_literals;

using tgw::bridge::MessageSendOutcome;
using tgw::bridge::MessageSendTracker;

namespace {

// Обёртка над trantor::EventLoop с явной синхронизацией старта (эталон из
// message_send_tracker_test.cpp): eventfd создаётся на потоке петли, указатель публикуется под
// мьютексом + cv (release/acquire) — постить в петлю можно только после его получения. Останов —
// quit() изнутри петли через queueInLoop + join.
class LoopThread {
   public:
    LoopThread() {
        thread_ = std::thread([this] {
            trantor::EventLoop loop;  // eventfd создаётся здесь, на этом потоке
            {
                std::lock_guard<std::mutex> lock(mutex_);
                loop_ = &loop;
            }
            cv_.notify_one();
            loop.loop();  // крутится до quit(); EventLoop разрушится здесь же, на своём потоке
        });
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return loop_ != nullptr; });
    }
    ~LoopThread() {
        loop_->queueInLoop([loop = loop_] { loop->quit(); });
        thread_.join();
    }

    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;

    // loop_ после конструктора больше не меняется — читать без лока безопасно.
    trantor::EventLoop* loop() const { return loop_; }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    trantor::EventLoop* loop_ = nullptr;
    std::thread thread_;
};

// Планирует корутину co_await tracker.waitFor(id, timeout) на петле и возвращает future исхода
// (как в HTTP-контексте). Результат читаем только после future.wait — promise/future даёт
// happens-before, гонки на MessageSendOutcome нет.
std::future<std::optional<MessageSendOutcome>> waitOnLoop(trantor::EventLoop* loop,
                                                          MessageSendTracker& tracker,
                                                          std::int64_t id,
                                                          std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<std::optional<MessageSendOutcome>>>();
    auto future = promise->get_future();
    loop->queueInLoop([&tracker, id, timeout, promise]() {
        [](MessageSendTracker& t, std::int64_t mid, std::chrono::milliseconds to,
           std::shared_ptr<std::promise<std::optional<MessageSendOutcome>>> result)
            -> drogon::AsyncTask {
            auto outcome = co_await t.waitFor(mid, to);
            result->set_value(std::move(outcome));
            co_return;
        }(tracker, id, timeout, promise);
    });
    return future;
}

}  // namespace

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

// updateMessageSendSucceeded резолвит трекер телом сообщения И при этом форвардится в WS как
// раньше (резолв добавлен ПЕРЕД веткой buildForwardable, а не вместо неё).
TEST(UpdateRouter, MessageSendSucceededResolvesTracker) {
    tgw::auth::AuthStateManager auth;
    tgw::ws::UpdateRouter router(auth, "sid");
    MessageSendTracker tracker;
    router.setMessageSendTracker(tracker);

    // event_publisher_ — прокси проверки forwardable-пути: дёргается из потока-приёмника ПОСЛЕ
    // WS fan-out (см. onUpdate). Пишем под мьютексом, читаем после receiver.join().
    std::mutex fmutex;
    std::string forwarded_payload;
    router.setEventPublisher([&](const std::string&, const std::string& payload) {
        std::lock_guard<std::mutex> lock(fmutex);
        forwarded_payload = payload;
    });

    LoopThread loop_thread;
    auto future = waitOnLoop(loop_thread.loop(), tracker, 42, 2s);

    // Приёмник (эмуляция потока TDLib): убеждаемся, что ожидание зарегистрировано, затем
    // скармливаем апдейт роутеру — onUpdate резолвит трекер и форвардит апдейт.
    std::thread receiver([&] {
        std::this_thread::sleep_for(50ms);
        auto msg = api::make_object<api::message>();
        msg->id_ = 100;
        msg->chat_id_ = -100;
        auto upd = api::make_object<api::updateMessageSendSucceeded>();
        upd->old_message_id_ = 42;
        upd->message_ = std::move(msg);
        router.onUpdate(std::move(upd));
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto outcome = future.get();
    receiver.join();

    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->succeeded);
    EXPECT_EQ(outcome->message["id"].asString(), "100");

    // WS-форвард сработал как раньше — резолв трекера его не вытеснил.
    std::lock_guard<std::mutex> lock(fmutex);
    EXPECT_NE(forwarded_payload.find("updateMessageSendSucceeded"), std::string::npos);
}

// updateMessageSendFailed резолвит трекер с полями ошибки (error_->code_/message_).
TEST(UpdateRouter, MessageSendFailedResolvesTrackerWithError) {
    tgw::auth::AuthStateManager auth;
    tgw::ws::UpdateRouter router(auth, "sid");
    MessageSendTracker tracker;
    router.setMessageSendTracker(tracker);

    LoopThread loop_thread;
    auto future = waitOnLoop(loop_thread.loop(), tracker, 42, 2s);

    std::thread receiver([&] {
        std::this_thread::sleep_for(50ms);
        auto err = api::make_object<api::error>();
        err->code_ = 400;
        err->message_ = "BAD";
        auto upd = api::make_object<api::updateMessageSendFailed>();
        upd->old_message_id_ = 42;
        upd->error_ = std::move(err);
        router.onUpdate(std::move(upd));
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto outcome = future.get();
    receiver.join();

    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome->succeeded);
    EXPECT_EQ(outcome->error_code, 400);
    EXPECT_EQ(outcome->error_message, "BAD");
}

// Без setMessageSendTracker(...) апдейты отправки обрабатываются как раньше и не падают.
TEST(UpdateRouter, MessageSendTrackerAbsentDoesNotCrash) {
    tgw::auth::AuthStateManager auth;
    tgw::ws::UpdateRouter router(auth, "sid");

    auto msg = api::make_object<api::message>();
    msg->id_ = 100;
    msg->chat_id_ = -100;
    auto ok = api::make_object<api::updateMessageSendSucceeded>();
    ok->old_message_id_ = 42;
    ok->message_ = std::move(msg);
    router.onUpdate(std::move(ok));

    auto err = api::make_object<api::error>();
    err->code_ = 400;
    err->message_ = "BAD";
    auto fail = api::make_object<api::updateMessageSendFailed>();
    fail->old_message_id_ = 42;
    fail->error_ = std::move(err);
    router.onUpdate(std::move(fail));

    SUCCEED();
}
