#include "ws/ws_registry.hpp"

#include <drogon/WebSocketConnection.h>
#include <trantor/net/InetAddress.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using tgw::ws::WsSubscriberRegistry;

namespace {

// drogon::WebSocketConnection — чисто абстрактный класс (все методы pure virtual), поэтому шов
// для теста реестра уже существует: подменяем реальное соединение фейком без сети/drogon-петли.
// Реестр использует connected()/send()/forceClose()/peerAddr() — их и инструментируем.
class FakeWsConnection final : public drogon::WebSocketConnection {
   public:
    void send(const char* msg, uint64_t len,
              drogon::WebSocketMessageType /*type*/ = drogon::WebSocketMessageType::Text) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_.emplace_back(msg, static_cast<std::size_t>(len));
    }
    void send(std::string_view msg,
              drogon::WebSocketMessageType /*type*/ = drogon::WebSocketMessageType::Text) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_.emplace_back(msg);
    }
    void sendJson(const Json::Value& /*json*/, drogon::WebSocketMessageType /*type*/ =
                                                   drogon::WebSocketMessageType::Text) override {}
    const trantor::InetAddress& localAddr() const override { return addr_; }
    const trantor::InetAddress& peerAddr() const override { return addr_; }
    bool connected() const override { return connected_.load(std::memory_order_relaxed); }
    bool disconnected() const override { return !connected_.load(std::memory_order_relaxed); }
    void shutdown(drogon::CloseCode /*code*/ = drogon::CloseCode::kNormalClosure,
                  const std::string& /*reason*/ = "") override {
        connected_.store(false, std::memory_order_relaxed);
    }
    void forceClose() override {
        force_closed_.store(true, std::memory_order_relaxed);
        connected_.store(false, std::memory_order_relaxed);
    }
    void setPingMessage(const std::string& /*message*/,
                        const std::chrono::duration<double>& /*interval*/) override {}
    void disablePing() override {}

    void setConnected(bool value) { connected_.store(value, std::memory_order_relaxed); }
    bool forceClosed() const { return force_closed_.load(std::memory_order_relaxed); }
    std::size_t sentCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_.size();
    }

   private:
    mutable std::mutex mutex_;
    std::vector<std::string> sent_;
    std::atomic<bool> connected_{true};
    std::atomic<bool> force_closed_{false};
    trantor::InetAddress addr_;
};

// Реестр — процесс-синглтон. Тесты работают от базового размера (дельты), а в TearDown снимают
// свои соединения и возвращают дефолтный лимит — чтобы тесты не зависели от порядка запуска.
class WsRegistryTest : public ::testing::Test {
   protected:
    WsSubscriberRegistry& reg = WsSubscriberRegistry::instance();
    std::vector<std::shared_ptr<FakeWsConnection>> conns_;

    void TearDown() override {
        for (const auto& c : conns_) {
            reg.disconnect(c);
        }
        reg.setMaxPendingBytes(8ULL * 1024 * 1024);  // дефолт из ws_registry.hpp
    }

    std::shared_ptr<FakeWsConnection> add() {
        auto c = std::make_shared<FakeWsConnection>();
        conns_.push_back(c);
        reg.connect(c);
        return c;
    }
};

}  // namespace

TEST_F(WsRegistryTest, ConnectAndDisconnectTrackSize) {
    const auto base = reg.size();
    auto c1 = add();
    auto c2 = add();
    EXPECT_EQ(reg.size(), base + 2);

    reg.disconnect(c1);
    EXPECT_EQ(reg.size(), base + 1);
}

// fanOut доставляет всем подключённым и пропускает отключённые (conn->connected()==false).
TEST_F(WsRegistryTest, FanOutSkipsDisconnected) {
    auto alive = add();
    auto dead = add();
    dead->setConnected(false);

    reg.fanOut("updateNewMessage", "hello");

    EXPECT_EQ(alive->sentCount(), 1u);
    EXPECT_EQ(dead->sentCount(), 0u);
}

// Фильтр подписки: тип не в наборе -> short-circuit (не шлём и в backlog не считаем).
TEST_F(WsRegistryTest, FilterShortCircuitsUnsubscribedTypes) {
    auto c = add();
    reg.setFilter(c, {"updateNewMessage"});

    reg.fanOut("updateDeleteMessages", "x");  // не подписан -> пропуск
    EXPECT_EQ(c->sentCount(), 0u);

    reg.fanOut("updateNewMessage", "y");  // подписан -> доставка
    EXPECT_EQ(c->sentCount(), 1u);
}

// Back-pressure: накопленные с последнего pong байты превысили лимит -> forceClose + удаление.
TEST_F(WsRegistryTest, SlowClientDisconnectedOnPendingBytesLimit) {
    const auto base = reg.size();
    reg.setMaxPendingBytes(10);
    auto c = add();

    reg.fanOut("t", "12345");  // 5 байт, в пределах лимита -> доставка
    EXPECT_EQ(c->sentCount(), 1u);
    EXPECT_FALSE(c->forceClosed());

    reg.fanOut("t", "123456");  // +6 = 11 > 10 -> отключение стального клиента
    EXPECT_TRUE(c->forceClosed());
    EXPECT_EQ(c->sentCount(), 1u);  // второй payload медленному клиенту НЕ отправлен
    EXPECT_EQ(reg.size(), base);  // соединение удалено из реестра
}

// notePong сбрасывает счётчик backlog'а: клиент, вычитывающий поток, не отключается.
TEST_F(WsRegistryTest, NotePongResetsBacklogCounter) {
    reg.setMaxPendingBytes(10);
    auto c = add();

    reg.fanOut("t", "12345");   // байтов = 5
    reg.notePong(c);            // pong -> счётчик обнулён
    reg.fanOut("t", "123456");  // байтов = 6 (а не 11) -> в пределах лимита

    EXPECT_FALSE(c->forceClosed());
    EXPECT_EQ(c->sentCount(), 2u);
}

// connect/disconnect из основного потока против fanOut из другого — без гонок/крашей (TSan/ASan).
// Снимок в fanOut берётся под локом, а send() идёт вне лока по shared_ptr из снимка — объект
// соединения жив, даже если параллельно disconnect убрал его из реестра.
TEST_F(WsRegistryTest, ConcurrentFanOutAndConnectDisconnect) {
    const auto base = reg.size();
    reg.setMaxPendingBytes(0);  // back-pressure выключен, чтобы fanOut не удалял соединения сам

    std::atomic<bool> stop{false};
    std::thread fanner([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            reg.fanOut("updateNewMessage", "payload-bytes");
        }
    });

    std::vector<std::shared_ptr<FakeWsConnection>> local;
    for (int i = 0; i < 200; ++i) {
        auto c = std::make_shared<FakeWsConnection>();
        reg.connect(c);
        local.push_back(c);
    }
    for (const auto& c : local) {
        reg.disconnect(c);
    }

    stop.store(true, std::memory_order_relaxed);
    fanner.join();

    EXPECT_EQ(reg.size(), base);  // все локальные соединения сняты
}
