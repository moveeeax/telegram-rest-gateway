#pragma once

#include <cstdint>

namespace tgw::config {
struct Config;
}
namespace tgw::bridge {
class TdBridge;
}

namespace tgw::auth {

class AuthStateManager;

// Автоподъём сессии при старте процесса ДО приёма HTTP (§7.1): глушит лог TDLib,
// толкает поток updateAuthorizationState и при WaitTdlibParameters автоматически шлёт
// setTdlibParameters из конфига. Дальше автомат идёт сам (Ready при сохранённой сессии
// либо WaitPhoneNumber). НЕ использует co_await (event-loop Drogon ещё не крутится):
// прогресс отслеживается по сменам состояния в AuthStateManager.
class StartupBootstrapper {
   public:
    // true — дошли до состояния != Unknown/WaitTdlibParameters (готовы принимать HTTP).
    static bool run(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                    const tgw::config::Config& config, AuthStateManager& auth);
};

}  // namespace tgw::auth
