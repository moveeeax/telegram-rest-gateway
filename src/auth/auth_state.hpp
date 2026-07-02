#pragma once

#include <td/telegram/td_api.h>

#include <cstdint>
#include <string_view>

namespace tgw::auth {

// Состояния автомата авторизации, достижимые в phone-flow MVP (§7.2). Всё прочее
// (wait_registration, wait_email*, QR-подтверждение и т.п.) — Unsupported => 409 AUTH_REQUIRED.
enum class AuthState : std::uint8_t {
    Unknown,  // ещё не получали updateAuthorizationState
    WaitTdlibParameters,
    WaitPhoneNumber,
    WaitCode,
    WaitPassword,
    Ready,
    LoggingOut,
    Closing,
    Closed,
    Unsupported,
};

inline AuthState authStateFromTdId(std::int32_t td_id) {
    namespace api = td::td_api;
    switch (td_id) {
        case api::authorizationStateWaitTdlibParameters::ID:
            return AuthState::WaitTdlibParameters;
        case api::authorizationStateWaitPhoneNumber::ID:
            return AuthState::WaitPhoneNumber;
        case api::authorizationStateWaitCode::ID:
            return AuthState::WaitCode;
        case api::authorizationStateWaitPassword::ID:
            return AuthState::WaitPassword;
        case api::authorizationStateReady::ID:
            return AuthState::Ready;
        case api::authorizationStateLoggingOut::ID:
            return AuthState::LoggingOut;
        case api::authorizationStateClosing::ID:
            return AuthState::Closing;
        case api::authorizationStateClosed::ID:
            return AuthState::Closed;
        default:
            return AuthState::Unsupported;
    }
}

// Стабильный строковый enum для DTO (§7.2).
inline std::string_view toString(AuthState s) {
    switch (s) {
        case AuthState::Unknown:
            return "unknown";
        case AuthState::WaitTdlibParameters:
            return "wait_tdlib_parameters";
        case AuthState::WaitPhoneNumber:
            return "wait_phone_number";
        case AuthState::WaitCode:
            return "wait_code";
        case AuthState::WaitPassword:
            return "wait_password";
        case AuthState::Ready:
            return "ready";
        case AuthState::LoggingOut:
            return "logging_out";
        case AuthState::Closing:
            return "closing";
        case AuthState::Closed:
            return "closed";
        case AuthState::Unsupported:
            return "unsupported_state";
    }
    return "unknown";
}

}  // namespace tgw::auth
