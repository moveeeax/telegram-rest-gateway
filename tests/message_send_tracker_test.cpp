#include "bridge/message_send_tracker.hpp"

#include "loop_thread.hpp"

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
#include <thread>

using namespace std::chrono_literals;

using tgw::bridge::MessageSendOutcome;
using tgw::bridge::MessageSendTracker;

namespace {

using tgw_test::LoopThread;

// Планирует корутину co_await tracker.waitFor(id, timeout) на петле и возвращает future исхода.
// Корутина обязана суспендиться на петле, чтобы resolve*()/таймаут вернули resume в неё (как в
// HTTP-контексте). Результат читаем ТОЛЬКО после future.wait — promise/future даёт happens-before,
// поэтому гонки на MessageSendOutcome нет. counter (если задан) считает число резюмирований
// корутины.
std::future<std::optional<MessageSendOutcome>> waitOnLoop(
    trantor::EventLoop* loop, MessageSendTracker& tracker, std::int64_t id,
    std::chrono::milliseconds timeout, std::shared_ptr<std::atomic<int>> counter = nullptr) {
    auto promise = std::make_shared<std::promise<std::optional<MessageSendOutcome>>>();
    auto future = promise->get_future();
    loop->queueInLoop([&tracker, id, timeout, promise, counter]() {
        [](MessageSendTracker& t, std::int64_t mid, std::chrono::milliseconds to,
           std::shared_ptr<std::promise<std::optional<MessageSendOutcome>>> result,
           std::shared_ptr<std::atomic<int>> resumes) -> drogon::AsyncTask {
            auto outcome = co_await t.waitFor(mid, to);
            if (resumes) {
                resumes->fetch_add(1, std::memory_order_relaxed);
            }
            result->set_value(std::move(outcome));
            co_return;
        }(tracker, id, timeout, promise, counter);
    });
    return future;
}

Json::Value sampleMessage() {
    Json::Value msg;
    msg["id"] = 100;
    msg["text"] = "hi";
    return msg;
}

}  // namespace

// Резолв из отдельного потока (эмуляция приёмника TDLib) ДО таймаута -> succeeded с телом.
TEST(MessageSendTracker, ResolvesBeforeTimeoutReturnsSucceeded) {
    MessageSendTracker tracker;
    LoopThread loop_thread;

    auto future = waitOnLoop(loop_thread.loop(), tracker, 42, 2s);

    // Из ОТДЕЛЬНОГО потока: убеждаемся, что ожидание уже зарегистрировано (фиксированная
    // небольшая задержка здесь ДОПУСТИМА — проверяем именно межпоточный резолв), затем резолвим.
    std::thread receiver([&] {
        std::this_thread::sleep_for(50ms);
        tracker.resolveSucceeded(42, sampleMessage());
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto outcome = future.get();
    receiver.join();

    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->succeeded);
    EXPECT_EQ(outcome->message, sampleMessage());
}

// Таймаут без резолва -> nullopt после ~50мс.
TEST(MessageSendTracker, TimeoutReturnsNulloptWhenNoResolve) {
    MessageSendTracker tracker;
    LoopThread loop_thread;

    auto future = waitOnLoop(loop_thread.loop(), tracker, 42, 50ms);

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto outcome = future.get();
    EXPECT_FALSE(outcome.has_value());
}

// resolveFailed до таймаута -> succeeded==false, поля ошибки проброшены.
TEST(MessageSendTracker, ResolveFailedReturnsErrorFields) {
    MessageSendTracker tracker;
    LoopThread loop_thread;

    auto future = waitOnLoop(loop_thread.loop(), tracker, 42, 2s);

    std::thread receiver([&] {
        std::this_thread::sleep_for(50ms);
        tracker.resolveFailed(42, 400, "FAILED");
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto outcome = future.get();
    receiver.join();

    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome->succeeded);
    EXPECT_EQ(outcome->error_code, 400);
    EXPECT_EQ(outcome->error_message, "FAILED");
}

// resolve для неизвестного id — no-op: не падает и не влияет на другое ожидание.
TEST(MessageSendTracker, ResolveForUnknownIdIsNoOp) {
    MessageSendTracker tracker;
    LoopThread loop_thread;

    auto future = waitOnLoop(loop_thread.loop(), tracker, 42, 2s);

    std::thread receiver([&] {
        std::this_thread::sleep_for(50ms);
        tracker.resolveSucceeded(999, sampleMessage());  // никто не ждёт 999 — no-op
        tracker.resolveSucceeded(42, sampleMessage());  // реальное ожидание не задето
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto outcome = future.get();
    receiver.join();

    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->succeeded);
}

// Резолв ПОСЛЕ уже сработавшего таймаута — no-op: корутина резюмирована ровно один раз (тот же
// инвариант «резолвит ровно одна сторона», что у CorrelationMap/TdBridge).
TEST(MessageSendTracker, DoubleResolveDoesNotDoubleResume) {
    MessageSendTracker tracker;
    LoopThread loop_thread;
    auto resumes = std::make_shared<std::atomic<int>>(0);

    auto future = waitOnLoop(loop_thread.loop(), tracker, 42, 50ms, resumes);

    // Резолвим ПОЗЖЕ таймаута: к этому моменту узел уже забран timeout-веткой -> claim вернёт
    // nullptr -> resolveSucceeded это no-op, второго резюмирования не будет.
    std::thread receiver([&] {
        std::this_thread::sleep_for(150ms);
        tracker.resolveSucceeded(42, sampleMessage());
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto outcome = future.get();
    receiver.join();

    EXPECT_FALSE(outcome.has_value());  // выиграл таймаут
    EXPECT_EQ(resumes->load(std::memory_order_relaxed), 1);
}

// drainAll() на shutdown принудительно резолвит висящий waitFor() как таймаут (nullopt), НЕ
// дожидаясь реального таймера — таймаут здесь заведомо больше времени теста, поэтому future,
// готовый раньше своего дедлайна, доказывает, что резолвнул именно drainAll(), а не таймер.
TEST(MessageSendTracker, DrainAllForceResolvesPendingWaitAsTimeout) {
    MessageSendTracker tracker;
    LoopThread loop_thread;

    auto future = waitOnLoop(loop_thread.loop(), tracker, 42, 60s);

    // Дать корутине время дойти до await_suspend/tryInsert.
    std::this_thread::sleep_for(50ms);
    tracker.drainAll();

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const auto outcome = future.get();
    EXPECT_FALSE(outcome.has_value());  // nullopt — тот же сигнал, что у обычного таймаута
}
