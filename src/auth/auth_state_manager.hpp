#pragma once

#include "auth/auth_state.hpp"
#include "bridge/update_sink.hpp"

#include <td/telegram/td_api.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace tgw::auth {

// Держит текущее состояние авторизации, обновляемое ТОЛЬКО из updateAuthorizationState
// (request_id==0, глобальный апдейт — §7.3). Работает как IUpdateSink моста: auth-апдейты
// обрабатывает, прочие пока считает (WS fan-out — этап 4). Потокобезопасен: пишет
// поток-приёмник, читают HTTP-потоки.
class AuthStateManager final : public tgw::bridge::IUpdateSink {
   public:
    void onUpdate(td::td_api::object_ptr<td::td_api::Object> update) override;

    AuthState current() const { return state_.load(std::memory_order_acquire); }
    std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    std::uint64_t updateCount() const { return update_count_.load(std::memory_order_relaxed); }

    // Ждёт первого updateAuthorizationState (generation > 0). true — дождались.
    bool waitForFirst(std::chrono::milliseconds timeout);
    // Ждёт смены поколения относительно prev. true — сменилось.
    bool waitForChange(std::uint64_t prev_generation, std::chrono::milliseconds timeout);

   private:
    void setState(AuthState s);

    std::atomic<AuthState> state_{AuthState::Unknown};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> update_count_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace tgw::auth
