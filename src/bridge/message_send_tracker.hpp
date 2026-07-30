#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace trantor {
class EventLoop;
}

namespace tgw::bridge {

// Внутренняя деталь ожидания (определение — в .cpp).
struct SendWaitState;

// Исход отправки, спроецированный из updateMessageSendSucceeded/Failed. Трекер намеренно работает
// только с уже готовым Json::Value (как ContextBuilder в вебхуках проецирует message СРАЗУ), не
// храня move-only td_api-объекты через границу async-компонента — отсюда полная независимость от
// td_api-типов.
struct MessageSendOutcome {
    bool succeeded = false;
    Json::Value message;          // dto::toJson(*upd.message_), если succeeded == true
    std::int32_t error_code = 0;  // upd.error_->code_, если succeeded == false
    std::string error_message;    // upd.error_->message_, если succeeded == false
};

// Сопоставление old_message_id -> результат отправки. Резолвит РОВНО ОДНА сторона — либо
// resolveSucceeded/resolveFailed (поток-приёмник TDLib), либо истечение таймаута (петля
// вызывающего): атомарный find+erase под общим мьютексом, как в CorrelationMap
// (src/bridge/correlation_map.cpp). Без max_inflight-лимита — ресурс не блокирующий, его темп
// естественно ограничен темпом HTTP-запросов.
class MessageSendTracker {
   public:
    class Awaitable {
       public:
        Awaitable(MessageSendTracker& tracker, std::int64_t old_message_id,
                  std::chrono::milliseconds timeout);
        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle);
        // nullopt — таймаут истёк без резолва.
        std::optional<MessageSendOutcome> await_resume();

       private:
        MessageSendTracker& tracker_;
        std::int64_t old_message_id_;
        std::chrono::milliseconds timeout_;
        std::shared_ptr<SendWaitState> state_;
    };

    Awaitable waitFor(std::int64_t old_message_id, std::chrono::milliseconds timeout) {
        return Awaitable(*this, old_message_id, timeout);
    }

    // Вызываются из потока-приёмника TDLib (UpdateRouter::onUpdate). No-op, если никто не ждёт
    // этот old_message_id (обычный случай при выключенном флаге — один промах в чужой/пустой
    // карте).
    void resolveSucceeded(std::int64_t old_message_id, Json::Value message_json);
    void resolveFailed(std::int64_t old_message_id, std::int32_t error_code,
                       std::string error_message);

   private:
    friend class Awaitable;
    bool tryInsert(std::int64_t old_message_id, std::shared_ptr<SendWaitState> state);
    std::shared_ptr<SendWaitState> claim(std::int64_t old_message_id);
    void resolveWith(std::int64_t old_message_id, MessageSendOutcome outcome);

    std::mutex mutex_;
    std::unordered_map<std::int64_t, std::shared_ptr<SendWaitState>> map_;
};

}  // namespace tgw::bridge
