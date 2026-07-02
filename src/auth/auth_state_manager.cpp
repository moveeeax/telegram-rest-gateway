#include "auth/auth_state_manager.hpp"

namespace tgw::auth {

namespace api = td::td_api;

void AuthStateManager::onUpdate(api::object_ptr<api::Object> update) {
    if (update == nullptr) {
        return;
    }
    if (update->get_id() == api::updateAuthorizationState::ID) {
        auto& upd = static_cast<api::updateAuthorizationState&>(*update);
        if (upd.authorization_state_ != nullptr) {
            setState(authStateFromTdId(upd.authorization_state_->get_id()));
        }
        return;
    }
    // Прочие апдейты на этапе 2 только считаем; WS fan-out — этап 4.
    update_count_.fetch_add(1, std::memory_order_relaxed);
}

void AuthStateManager::setState(AuthState s) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.store(s, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    cv_.notify_all();
}

bool AuthStateManager::waitForFirst(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this] { return generation_.load(std::memory_order_acquire) > 0; });
}

bool AuthStateManager::waitForChange(std::uint64_t prev_generation,
                                     std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, prev_generation] {
        return generation_.load(std::memory_order_acquire) > prev_generation;
    });
}

}  // namespace tgw::auth
