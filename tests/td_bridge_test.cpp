#include "bridge/td_bridge.hpp"

#include "bridge/clock.hpp"
#include "bridge/update_sink.hpp"
#include "fake_transport.hpp"

#include <drogon/utils/coroutine.h>
#include <td/telegram/td_api.h>
#include <trantor/net/EventLoop.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>

namespace td_api = td::td_api;
using namespace std::chrono_literals;

using tgw::bridge::BridgeConfig;
using tgw::bridge::CountingUpdateSink;
using tgw::bridge::TdBridge;
using tgw::testing::FakeTdTransport;

namespace {

// Атомарные часы: поток-приёмник моста читает время из СВОЕГО потока, тест сдвигает его из
// основного — держим точку в atomic, чтобы TSan не видел гонки на невыровненном чтении/записи.
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

// Обёртка над trantor::EventLoop с ЯВНОЙ синхронизацией старта/останова. У trantor::EventLoopThread
// этой версии getLoop() публикует лишь указатель на петлю, но НЕ устанавливает happens-before между
// конструктором EventLoop (где на потоке петли создаётся wakeup-eventfd) и первым queueInLoop из
// тест-треда — под TSan это гонка на eventfd (чтение fd main-тредом vs его создание на потоке
// петли). Здесь HB задаём сами: поток создаёт EventLoop, публикует указатель под мьютексом + cv, и
// только ПОСЛЕ того как тест-тред получил указатель через тот же мьютекс (release/acquire), он
// постит в петлю — создание eventfd гарантированно happens-before любого queueInLoop. Останов —
// quit() ИЗНУТРИ петли (через queueInLoop) + join: quit() не дёргается из чужого треда по
// крутящейся петле.
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
        // quit исполняется НА потоке петли -> нет гонки quit() с loop() из чужого треда.
        loop_->queueInLoop([loop = loop_] { loop->quit(); });
        thread_.join();
    }

    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;

    // loop_ после конструктора больше не меняется (запись под мьютексом до cv.wait), поэтому
    // читать его без лока безопасно.
    trantor::EventLoop* loop() const { return loop_; }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    trantor::EventLoop* loop_ = nullptr;
    std::thread thread_;
};

// Владеет транспортом, sink'ом, часами, event-loop'ом и мостом в порядке, гарантирующем
// корректный teardown под TSan: приёмник моста джойнится ДО разрушения транспорта, а петля
// гасится (в деструкторе LoopThread) после остановки моста. Порядок членов = порядок инициализации.
class BridgeHarness {
   public:
    explicit BridgeHarness(BridgeConfig config) : bridge_(transport_, sink_, config, clock_.fn()) {
        // loop_thread_ уже полностью запущен (его конструктор дождался готовности петли), поэтому
        // здесь достаточно поднять приёмник моста.
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

// Запускает co_await bridge.invoke(fn) на переданной петле и возвращает future результата.
// Корутина обязана суспендиться на петле, чтобы resolve() вернул resume в неё через queueInLoop
// (как в HTTP-контексте). Результат читаем ТОЛЬКО после future.wait — promise/future
// устанавливает happens-before, поэтому гонки на object_ptr нет.
std::future<td_api::object_ptr<td_api::Object>> invokeOnLoop(
    trantor::EventLoop* loop, TdBridge& bridge, std::int32_t client_id,
    td_api::object_ptr<td_api::Function> fn) {
    auto promise = std::make_shared<std::promise<td_api::object_ptr<td_api::Object>>>();
    auto future = promise->get_future();
    // fn move-only, а std::function в queueInLoop должна быть копируема — прячем в shared-холдер.
    auto fn_holder = std::make_shared<td_api::object_ptr<td_api::Function>>(std::move(fn));
    loop->queueInLoop([&bridge, client_id, promise, fn_holder]() {
        [](TdBridge& td, std::int32_t cid, td_api::object_ptr<td_api::Function> function,
           std::shared_ptr<std::promise<td_api::object_ptr<td_api::Object>>> result)
            -> drogon::AsyncTask {
            auto object = co_await td.invoke(cid, std::move(function));
            result->set_value(std::move(object));
            co_return;
        }(bridge, client_id, std::move(*fn_holder), promise);
    });
    return future;
}

// Ожидание условия с таймаутом (не «sleep-и-надейся»: цикл проверяет реальный предикат).
template <typename Predicate>
bool waitFor(Predicate pred, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

td_api::object_ptr<td_api::Function> getMe() {
    return td_api::make_object<td_api::getMe>();
}

const td_api::error& asError(const td_api::object_ptr<td_api::Object>& object) {
    return static_cast<const td_api::error&>(*object);
}

}  // namespace

// Happy-path: dispatch ответа по request_id резолвит подвешенную корутину этим ответом.
TEST(TdBridge, HappyPathResolvesWithResponse) {
    BridgeHarness h{BridgeConfig{}};
    const auto cid = h.bridge().createClientId();

    auto future = invokeOnLoop(h.loop(), h.bridge(), cid, getMe());
    // Ждём, пока корутина зарегистрировала запрос и отправила его в транспорт (узнаём request_id).
    ASSERT_TRUE(waitFor([&] { return !h.transport().sentRequestIds().empty(); }));
    const auto request_id = h.transport().sentRequestIds().front();

    h.transport().pushResponse(cid, request_id, td_api::make_object<td_api::ok>());

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto object = future.get();
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->get_id(), td_api::ok::ID);
    EXPECT_EQ(h.bridge().pending(), 0u);
}

// TTL-sweep резолвит просроченный запрос ошибкой 504 UPSTREAM_TIMEOUT; пришедший ПОЗЖЕ ответ на
// тот же request_id обязан быть дропнут (записи в корреляции уже нет) — без падения и без второго
// резолва (иначе resume уже завершённой корутины -> use-after-free, ловится ASan).
TEST(TdBridge, TimeoutSweepResolvesAndLateResponseDropped) {
    BridgeConfig config;
    config.ttl = 50ms;
    config.sweep_interval = 10ms;
    BridgeHarness h{config};
    const auto cid = h.bridge().createClientId();

    auto future = invokeOnLoop(h.loop(), h.bridge(), cid, getMe());
    ASSERT_TRUE(waitFor([&] { return !h.transport().sentRequestIds().empty(); }));
    const auto request_id = h.transport().sentRequestIds().front();
    EXPECT_EQ(h.bridge().pending(), 1u);  // часы заморожены -> sweep пока не срабатывает

    h.clock().advance(200ms);  // за TTL -> следующий проход sweep извлечёт запись

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto object = future.get();
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(object->get_id(), td_api::error::ID);
    EXPECT_EQ(asError(object).code_, 504);
    EXPECT_EQ(asError(object).message_, "UPSTREAM_TIMEOUT");
    EXPECT_EQ(h.bridge().pending(), 0u);

    // Поздний ответ на уже истёкший request_id: должен быть тихо отброшен.
    const auto before = h.bridge().receiveIterations();
    h.transport().pushResponse(cid, request_id, td_api::make_object<td_api::ok>());
    ASSERT_TRUE(waitFor([&] { return h.bridge().receiveIterations() > before + 1; }));
    EXPECT_EQ(h.bridge().pending(), 0u);  // ничего не воскресло, корреляция пуста
}

// Превышение лимита in-flight -> запрос немедленно резолвится 503 SERVICE_BUSY, до транспорта
// он не доходит (td_bridge.cpp: await_suspend возвращает false).
TEST(TdBridge, InflightLimitReturnsServiceBusy) {
    BridgeConfig config;
    config.max_inflight = 1;
    BridgeHarness h{config};
    const auto cid = h.bridge().createClientId();

    // Первый запрос занимает единственный слот и подвисает (ответа не шлём).
    auto first = invokeOnLoop(h.loop(), h.bridge(), cid, getMe());
    ASSERT_TRUE(waitFor([&] { return h.bridge().pending() == 1u; }));

    // Второй — лимит исчерпан: моментальный 503.
    auto second = invokeOnLoop(h.loop(), h.bridge(), cid, getMe());
    ASSERT_EQ(second.wait_for(2s), std::future_status::ready);
    auto rejected = second.get();
    ASSERT_NE(rejected, nullptr);
    ASSERT_EQ(rejected->get_id(), td_api::error::ID);
    EXPECT_EQ(asError(rejected).code_, 503);
    EXPECT_EQ(asError(rejected).message_, "SERVICE_BUSY");
    EXPECT_EQ(h.transport().sentRequestIds().size(), 1u);  // отвергнутый не отправлялся

    // Завершаем первый, чтобы корутина не осталась подвешенной к моменту teardown.
    const auto request_id = h.transport().sentRequestIds().front();
    h.transport().pushResponse(cid, request_id, td_api::make_object<td_api::ok>());
    ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
    (void)first.get();
}

// Апдейт (request_id == 0) уходит в sink, а не в корреляцию.
TEST(TdBridge, UpdateIsRoutedToSink) {
    BridgeHarness h{BridgeConfig{}};
    const auto cid = h.bridge().createClientId();

    EXPECT_EQ(h.sink().count(), 0u);
    h.transport().pushUpdate(cid, td_api::make_object<td_api::ok>());

    ASSERT_TRUE(waitFor([&] { return h.sink().count() == 1u; }));
    EXPECT_EQ(h.bridge().pending(), 0u);  // апдейт не создаёт записи корреляции
}
