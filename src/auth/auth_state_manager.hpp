#pragma once

#include "auth/auth_state.hpp"
#include "bridge/update_sink.hpp"

#include <td/telegram/td_api.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

namespace tgw::auth {

// Держит текущее состояние авторизации, обновляемое ТОЛЬКО из updateAuthorizationState
// (request_id==0, глобальный апдейт — §7.3). Работает как IUpdateSink моста: auth-апдейты
// обрабатывает, прочие пока считает (WS fan-out — этап 4). Потокобезопасен: пишет
// поток-приёмник, читают HTTP-потоки.
// Данные authenticationCodeInfo из wait_code (§7.2): куда отправлен код и когда resend.
struct CodeInfo {
    std::string type;       // sms | telegram_message | call | ...
    std::string next_type;  // тип после resend ("" если нет)
    std::int32_t length = 0;
    std::int32_t timeout = 0;  // секунд до разрешённого resend
};

class AuthStateManager final : public tgw::bridge::IUpdateSink {
   public:
    void onUpdate(td::td_api::object_ptr<td::td_api::Object> update) override;

    // Снимок code_info (валиден в wait_code; иначе пустой type).
    CodeInfo codeInfo() const;
    // tg://login-ссылка (валидна в wait_other_device_confirmation; TDLib её обновляет).
    std::string qrLink() const;

    AuthState current() const { return state_.load(std::memory_order_acquire); }
    std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    std::uint64_t updateCount() const { return update_count_.load(std::memory_order_relaxed); }

    // Ждёт первого updateAuthorizationState (generation > 0). true — дождались.
    bool waitForFirst(std::chrono::milliseconds timeout);
    // Ждёт смены поколения относительно prev. true — сменилось.
    bool waitForChange(std::uint64_t prev_generation, std::chrono::milliseconds timeout);
    // Ждёт перехода в Closed (для graceful shutdown: TDLib должен закрыть БД). true — дождались.
    bool waitForClosed(std::chrono::milliseconds timeout);

    // Помечает, что завершение ожидаемо (main начинает shutdown) — колбэк ниже не сработает.
    void expectShutdown() { expect_shutdown_.store(true, std::memory_order_release); }
    // Колбэк на НЕожидаемый LoggingOut/Closing/Closed (удалённый logout с другого устройства,
    // AUTH_KEY_DUPLICATED и т.п.). Вызывается один раз из потока-приёмника.
    void setOnUnexpectedTermination(std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        on_unexpected_termination_ = std::move(cb);
    }

   private:
    void setState(AuthState s);

    std::atomic<AuthState> state_{AuthState::Unknown};
    std::atomic<bool> expect_shutdown_{false};
    std::atomic<bool> termination_reported_{false};
    std::function<void()> on_unexpected_termination_;  // под mutex_
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> update_count_{0};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    CodeInfo code_info_;   // под mutex_; заполняется в wait_code
    std::string qr_link_;  // под mutex_; заполняется в wait_other_device_confirmation
};

}  // namespace tgw::auth
