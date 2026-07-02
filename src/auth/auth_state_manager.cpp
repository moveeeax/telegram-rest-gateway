#include "auth/auth_state_manager.hpp"

namespace tgw::auth {

namespace api = td::td_api;

namespace {

// authenticationCodeType* -> строковый тип для DTO.
std::string codeTypeToString(const api::AuthenticationCodeType* type, std::int32_t& length_out) {
    if (type == nullptr) {
        return "";
    }
    switch (type->get_id()) {
        case api::authenticationCodeTypeTelegramMessage::ID:
            length_out =
                static_cast<const api::authenticationCodeTypeTelegramMessage&>(*type).length_;
            return "telegram_message";
        case api::authenticationCodeTypeSms::ID:
            length_out = static_cast<const api::authenticationCodeTypeSms&>(*type).length_;
            return "sms";
        case api::authenticationCodeTypeCall::ID:
            length_out = static_cast<const api::authenticationCodeTypeCall&>(*type).length_;
            return "call";
        case api::authenticationCodeTypeFlashCall::ID:
            return "flash_call";
        case api::authenticationCodeTypeMissedCall::ID:
            return "missed_call";
        case api::authenticationCodeTypeFragment::ID:
            return "fragment";
        default:
            return "other";
    }
}

}  // namespace

void AuthStateManager::onUpdate(api::object_ptr<api::Object> update) {
    if (update == nullptr) {
        return;
    }
    if (update->get_id() == api::updateAuthorizationState::ID) {
        auto& upd = static_cast<api::updateAuthorizationState&>(*update);
        if (upd.authorization_state_ != nullptr) {
            if (upd.authorization_state_->get_id() ==
                api::authorizationStateWaitOtherDeviceConfirmation::ID) {
                const auto& wait =
                    static_cast<const api::authorizationStateWaitOtherDeviceConfirmation&>(
                        *upd.authorization_state_);
                std::lock_guard<std::mutex> lock(mutex_);
                qr_link_ = wait.link_;
            }
            if (upd.authorization_state_->get_id() == api::authorizationStateWaitCode::ID) {
                const auto& wait =
                    static_cast<const api::authorizationStateWaitCode&>(*upd.authorization_state_);
                CodeInfo info;
                if (wait.code_info_ != nullptr) {
                    info.type = codeTypeToString(wait.code_info_->type_.get(), info.length);
                    std::int32_t unused = 0;
                    info.next_type = codeTypeToString(wait.code_info_->next_type_.get(), unused);
                    info.timeout = wait.code_info_->timeout_;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                code_info_ = std::move(info);
            }
            setState(authStateFromTdId(upd.authorization_state_->get_id()));
        }
        return;
    }
    // Прочие апдейты на этапе 2 только считаем; WS fan-out — этап 4.
    update_count_.fetch_add(1, std::memory_order_relaxed);
}

void AuthStateManager::setState(AuthState s) {
    std::function<void()> terminate_cb;
    std::function<void()> ready_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.store(s, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        // Удалённый logout / отзыв сессии: терминальное состояние пришло, хотя main shutdown
        // не начинал. Один раз дёргаем колбэк (main по нему останавливает сервис — иначе он
        // молча продолжил бы отдавать 409 на всё).
        const bool terminal =
            (s == AuthState::LoggingOut || s == AuthState::Closing || s == AuthState::Closed);
        if (terminal && !expect_shutdown_.load(std::memory_order_acquire) &&
            !termination_reported_.exchange(true) && on_unexpected_termination_) {
            terminate_cb = on_unexpected_termination_;
        }
        if (s == AuthState::Ready && !ready_reported_.exchange(true) && on_ready_) {
            ready_cb = on_ready_;
        }
    }
    cv_.notify_all();
    if (terminate_cb) {
        terminate_cb();
    }
    if (ready_cb) {
        ready_cb();
    }
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

CodeInfo AuthStateManager::codeInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return code_info_;
}

std::string AuthStateManager::qrLink() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return qr_link_;
}

bool AuthStateManager::waitForClosed(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] {
        return state_.load(std::memory_order_acquire) == AuthState::Closed;
    });
}

}  // namespace tgw::auth
