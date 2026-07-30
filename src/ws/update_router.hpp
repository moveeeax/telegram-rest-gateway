#pragma once

#include "bridge/update_sink.hpp"

#include <td/telegram/td_api.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <json/value.h>
#include <optional>
#include <string>
#include <utility>

namespace tgw::auth {
class AuthStateManager;
}

namespace tgw::bridge {
class MessageSendTracker;
}

namespace tgw::ws {

// Прикладной апдейт, готовый к отдаче наружу.
struct ForwardableUpdate {
    std::string update_type;
    Json::Value data;
};

// Чистая функция (тестируемая): проецирует апдейт TDLib в наружный формат, если он в
// allowlist прикладных апдейтов (§6.4). nullopt — служебный/непроецируемый апдейт.
// updateAuthorizationState здесь НЕ обрабатывается (его роутит UpdateRouter в AuthStateManager).
std::optional<ForwardableUpdate> buildForwardable(const td::td_api::Object& update);

// Приёмник апдейтов моста: авторизационные -> AuthStateManager, прикладные -> WS fan-out
// (§6.3′). Служебные (option/connectionState) отбрасываются.
class UpdateRouter final : public tgw::bridge::IUpdateSink {
   public:
    // session_id — метка аккаунта (TGW_SESSION_ID): идёт в кадры WS и события Kafka.
    explicit UpdateRouter(tgw::auth::AuthStateManager& auth, std::string session_id = "default")
        : auth_(auth), session_id_(std::move(session_id)) {}

    void onUpdate(td::td_api::object_ptr<td::td_api::Object> update) override;

    // Дополнительный издатель событий (Kafka и т.п.): (key, payload). Вызывается из
    // потока-приёмника ПОСЛЕ WS fan-out — обязан быть неблокирующим. Задавать до start().
    void setEventPublisher(std::function<void(const std::string&, const std::string&)> pub) {
        event_publisher_ = std::move(pub);
    }

    // Колбэк на переход соединения в connectionStateReady (в т.ч. после реконнекта). Служит
    // для keep-online (переустановка setOption("online", true)). Вызывается из потока-приёмника
    // при КАЖДОМ Ready. UpdateRouter не зависит от TdBridge — само действие инкапсулирует
    // колбэк (тестируемость). Задавать до start(): пишется до старта потока, читается после —
    // гонки на самом std::function нет (как у event_publisher_).
    void setOnConnectionReady(std::function<void()> cb) { on_connection_ready_ = std::move(cb); }

    // Трекер отправки: резолв updateMessageSendSucceeded/Failed по old_message_id (§ humanize
    // typing). Опциональный, как event_publisher_/on_connection_ready_ — если не задан, резолв не
    // вызывается, а WS-форворд этих апдейтов работает как раньше. Задавать до start(): пишется до
    // старта потока-приёмника, читается после (гонки на самом указателе нет). Ссылка обязана
    // пережить UpdateRouter.
    void setMessageSendTracker(tgw::bridge::MessageSendTracker& tracker) {
        message_send_tracker_ = &tracker;
    }

   private:
    tgw::auth::AuthStateManager& auth_;
    std::string session_id_;
    std::function<void(const std::string&, const std::string&)> event_publisher_;
    std::function<void()> on_connection_ready_;
    tgw::bridge::MessageSendTracker* message_send_tracker_ = nullptr;
    std::atomic<std::uint64_t> seq_{0};
};

}  // namespace tgw::ws
