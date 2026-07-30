#include "webhook/context_builder.hpp"

#include "bridge/td_bridge.hpp"
#include "bridge/update_sink.hpp"
#include "ws/owner_mention_detector.hpp"
#include "fake_transport.hpp"

#include <drogon/utils/coroutine.h>
#include <td/telegram/td_api.h>
#include <trantor/net/EventLoop.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace td_api = td::td_api;
using namespace std::chrono_literals;

using tgw::bridge::BridgeConfig;
using tgw::bridge::CountingUpdateSink;
using tgw::bridge::TdBridge;
using tgw::testing::FakeTdTransport;
using tgw::webhook::WebhookEvent;
using tgw::ws::DetectResult;
using tgw::ws::TriggerReason;

namespace {

// --- Harness-паттерн, дословно перенесён из tests/td_bridge_test.cpp (TSan-проверенный) ------
// Эта задача корутинная и кросс-поточная, поэтому синхронизация старта петли и teardown должна
// повторять эталон один-в-один: явные mutex+cv на публикацию петли, quit() изнутри петли.

// Атомарные часы: поток-приёмник моста читает время из СВОЕГО потока — держим точку в atomic.
class FakeClock {
   public:
    tgw::bridge::SteadyClock fn() {
        return [this] { return now_.load(std::memory_order_relaxed); };
    }
    void advance(std::chrono::milliseconds delta) {
        now_.store(now_.load(std::memory_order_relaxed) + delta, std::memory_order_relaxed);
    }

   private:
    std::atomic<std::chrono::steady_clock::time_point> now_{std::chrono::steady_clock::now()};
};

// Обёртка над trantor::EventLoop с ЯВНОЙ синхронизацией старта/останова (см. td_bridge_test.cpp:
// happens-before на создание wakeup-eventfd против первого queueInLoop из тест-треда; quit()
// исполняется ИЗНУТРИ петли через queueInLoop, а не из чужого треда).
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

    trantor::EventLoop* loop() const { return loop_; }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    trantor::EventLoop* loop_ = nullptr;
    std::thread thread_;
};

// Владеет транспортом, sink'ом, часами, петлёй и мостом в порядке, гарантирующем корректный
// teardown под TSan (порядок членов = порядок инициализации; мост джойнится до транспорта).
class BridgeHarness {
   public:
    explicit BridgeHarness(BridgeConfig config) : bridge_(transport_, sink_, config, clock_.fn()) {
        bridge_.start();
    }
    ~BridgeHarness() { bridge_.stop(); }

    BridgeHarness(const BridgeHarness&) = delete;
    BridgeHarness& operator=(const BridgeHarness&) = delete;

    FakeTdTransport& transport() { return transport_; }
    CountingUpdateSink& sink() { return sink_; }
    FakeClock& clock() { return clock_; }
    TdBridge& bridge() { return bridge_; }
    trantor::EventLoop* loop() { return loop_thread_.loop(); }

   private:
    FakeTdTransport transport_;
    CountingUpdateSink sink_;
    FakeClock clock_;
    LoopThread loop_thread_;
    TdBridge bridge_;
};

// --- Скриптование цепочки getMessage ---------------------------------------------------------
// buildEvent поднимает reply-цепочку строго последовательно: следующий getMessage уходит лишь
// после резолва предыдущего (цепочка co_await). Поэтому i-й отправленный запрос получает i-й
// скрипт по порядку появления — сопоставлять по message_id не требуется. Скармливаем ответы из
// отдельного потока, чтобы не блокировать петлю моста; весь обмен с транспортом идёт под его
// внутренним мьютексом (HB для TSan), стоп-флаг — atomic, поток джойнится в деструкторе.
using ScriptFactory = std::function<td_api::object_ptr<td_api::Object>()>;

class ChainDriver {
   public:
    ChainDriver(FakeTdTransport& transport, std::int32_t client_id,
                std::vector<ScriptFactory> scripts)
        : transport_(transport), client_id_(client_id), scripts_(std::move(scripts)) {
        thread_ = std::thread([this] { run(); });
    }
    ~ChainDriver() {
        stop_.store(true, std::memory_order_relaxed);
        thread_.join();
    }

    ChainDriver(const ChainDriver&) = delete;
    ChainDriver& operator=(const ChainDriver&) = delete;

   private:
    void run() {
        std::size_t served = 0;
        while (served < scripts_.size() && !stop_.load(std::memory_order_relaxed)) {
            const auto ids = transport_.sentRequestIds();
            if (ids.size() > served) {
                transport_.pushResponse(client_id_, ids[served], scripts_[served]());
                ++served;
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
    }

    FakeTdTransport& transport_;
    std::int32_t client_id_;
    std::vector<ScriptFactory> scripts_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Сообщение-заглушка для скрипта: id/chat/автор/(опц.) reply_to. reply_to == 0 => корень цепочки.
td_api::object_ptr<td_api::message> makeMsg(std::int64_t id, std::int64_t chat_id,
                                            std::int64_t sender_user_id, std::int64_t reply_to) {
    auto m = td_api::make_object<td_api::message>();
    m->id_ = id;
    m->chat_id_ = chat_id;
    m->date_ = 1730000000;
    m->is_outgoing_ = false;
    m->sender_id_ = td_api::make_object<td_api::messageSenderUser>(sender_user_id);
    auto ft = td_api::make_object<td_api::formattedText>();
    ft->text_ = "m" + std::to_string(id);
    m->content_ = td_api::make_object<td_api::messageText>(std::move(ft), nullptr, nullptr);
    if (reply_to != 0) {
        auto reply = td_api::make_object<td_api::messageReplyToMessage>();
        reply->message_id_ = reply_to;
        m->reply_to_ = std::move(reply);
    }
    return m;
}

ScriptFactory msgScript(std::int64_t id, std::int64_t chat_id, std::int64_t sender,
                        std::int64_t reply_to) {
    return [=]() -> td_api::object_ptr<td_api::Object> {
        return makeMsg(id, chat_id, sender, reply_to);
    };
}

ScriptFactory errorScript(std::int32_t code, std::string message) {
    return [code, message]() -> td_api::object_ptr<td_api::Object> {
        return td_api::make_object<td_api::error>(code, message);
    };
}

// Гоняет buildEvent на петле моста и снимает результат через promise/future (HB на результат).
// Корутина суспендится на петле, поэтому resolve() возвращает resume в неё через queueInLoop —
// как в HTTP-контексте. msg живёт в теле теста до future.get(), поэтому передача по const& безопасна.
std::future<std::optional<WebhookEvent>> runBuildEvent(BridgeHarness& h, std::int32_t client_id,
                                                       const td_api::message& msg,
                                                       DetectResult det, std::int64_t owner_id,
                                                       std::string session_id,
                                                       std::int32_t received_at, int chain_limit) {
    auto promise = std::make_shared<std::promise<std::optional<WebhookEvent>>>();
    auto future = promise->get_future();
    h.loop()->queueInLoop([&h, client_id, &msg, det, owner_id, session_id, received_at, chain_limit,
                           promise]() {
        [](TdBridge& bridge, std::int32_t cid, const td_api::message& m, DetectResult d,
           std::int64_t owner, std::string sid, std::int32_t rat, int limit,
           std::shared_ptr<std::promise<std::optional<WebhookEvent>>> result) -> drogon::AsyncTask {
            auto ev = co_await tgw::webhook::buildEvent(bridge, cid, m, d, owner, std::move(sid),
                                                        rat, limit);
            result->set_value(std::move(ev));
            co_return;
        }(h.bridge(), client_id, msg, det, owner_id, session_id, received_at, chain_limit, promise);
    });
    return future;
}

}  // namespace

// Скрипт: incoming 4200 -> reply 4100 -> reply 4090 -> reply 4080 (корень). owner=111, родитель
// 4100 отправлен owner => reply подтверждён. Цепочка [4100,4090,4080] в порядке родитель->корень,
// без усечения.
TEST(ContextBuilder, ReplyChainToRootWithLimit) {
    BridgeHarness h{BridgeConfig{}};
    const auto cid = h.bridge().createClientId();

    const auto incoming = makeMsg(4200, -100500, 999, 4100);
    DetectResult det;
    det.triggered = false;
    det.reply_pending = true;
    det.reason = TriggerReason::Reply;

    ChainDriver driver(h.transport(), cid,
                       {msgScript(4100, -100500, 111, 4090),   // родитель = owner
                        msgScript(4090, -100500, 999, 4080),
                        msgScript(4080, -100500, 999, 0)});     // корень, reply нет

    auto future = runBuildEvent(h, cid, *incoming, det, /*owner=*/111, "sess", 1730000000, 20);
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const auto result = future.get();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->trigger_reason, "reply");
    EXPECT_FALSE(result->chain_truncated);
    ASSERT_TRUE(result->reply_chain.isArray());
    ASSERT_EQ(result->reply_chain.size(), 3u);
    EXPECT_EQ(result->reply_chain[0]["id"].asString(), "4100");  // родитель первым
    EXPECT_EQ(result->reply_chain[1]["id"].asString(), "4090");
    EXPECT_EQ(result->reply_chain[2]["id"].asString(), "4080");  // корень последним
    EXPECT_EQ(result->session_id, "sess");
    EXPECT_EQ(result->owner_id, "111");
    EXPECT_EQ(result->received_at, 1730000000);
    EXPECT_EQ(result->event_id, "sess:-100500:4200");
    EXPECT_EQ(result->message["id"].asString(), "4200");
}

// det.reply_pending, но родитель отправлен НЕ owner => триггер не подтверждён => nullopt.
TEST(ContextBuilder, ReplyNotFromOwnerReturnsNullopt) {
    BridgeHarness h{BridgeConfig{}};
    const auto cid = h.bridge().createClientId();

    const auto incoming = makeMsg(4200, -100500, 999, 4100);
    DetectResult det;
    det.triggered = false;
    det.reply_pending = true;
    det.reason = TriggerReason::Reply;

    ChainDriver driver(h.transport(), cid,
                       {msgScript(4100, -100500, /*sender=*/222, 4090)});  // не owner

    auto future = runBuildEvent(h, cid, *incoming, det, /*owner=*/111, "sess", 1730000000, 20);
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const auto result = future.get();

    EXPECT_FALSE(result.has_value());
}

// Цепочка длиннее лимита (limit=2): усечение до ровно 2 звеньев, chain_truncated=true. Триггер —
// mention (reply_pending=false), поэтому владельческая проверка не участвует: проверяем чистую
// механику лимита.
TEST(ContextBuilder, ChainTruncatedAtLimit) {
    BridgeHarness h{BridgeConfig{}};
    const auto cid = h.bridge().createClientId();

    const auto incoming = makeMsg(4200, -100500, 999, 4100);
    DetectResult det;
    det.triggered = true;
    det.reply_pending = false;
    det.reason = TriggerReason::Mention;

    // Достаточно 2 скриптов: при limit=2 buildEvent отправит ровно 2 getMessage.
    ChainDriver driver(h.transport(), cid,
                       {msgScript(4100, -100500, 999, 4090),
                        msgScript(4090, -100500, 999, 4080)});  // ещё есть родитель -> усекли

    auto future = runBuildEvent(h, cid, *incoming, det, /*owner=*/111, "sess", 1730000000, 2);
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const auto result = future.get();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->trigger_reason, "mention");
    EXPECT_TRUE(result->chain_truncated);
    ASSERT_TRUE(result->reply_chain.isArray());
    ASSERT_EQ(result->reply_chain.size(), 2u);
    EXPECT_EQ(result->reply_chain[0]["id"].asString(), "4100");
    EXPECT_EQ(result->reply_chain[1]["id"].asString(), "4090");
}

// getMessage вернул error на 2-м звене => частичная цепочка (1 звено) + chain_truncated=true.
// Первый родитель (4100) — owner, поэтому reply-триггер подтверждён и событие возвращается.
TEST(ContextBuilder, GetMessageFailureTruncates) {
    BridgeHarness h{BridgeConfig{}};
    const auto cid = h.bridge().createClientId();

    const auto incoming = makeMsg(4200, -100500, 999, 4100);
    DetectResult det;
    det.triggered = false;
    det.reply_pending = true;
    det.reason = TriggerReason::Reply;

    ChainDriver driver(h.transport(), cid,
                       {msgScript(4100, -100500, 111, 4090),   // owner, есть родитель 4090
                        errorScript(500, "BOOM")});            // 2-е звено падает

    auto future = runBuildEvent(h, cid, *incoming, det, /*owner=*/111, "sess", 1730000000, 20);
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const auto result = future.get();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->trigger_reason, "reply");
    EXPECT_TRUE(result->chain_truncated);
    ASSERT_TRUE(result->reply_chain.isArray());
    ASSERT_EQ(result->reply_chain.size(), 1u);
    EXPECT_EQ(result->reply_chain[0]["id"].asString(), "4100");
}
