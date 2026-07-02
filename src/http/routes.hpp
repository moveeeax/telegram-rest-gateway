#pragma once

#include <cstdint>

namespace tgw::bridge {
class TdBridge;
}
namespace tgw::auth {
class AuthStateManager;
}

namespace tgw::http {

// Регистрирует REST-маршруты этапов 1–2: health, ready, me, auth/*.
void registerRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                    tgw::auth::AuthStateManager& auth);

}  // namespace tgw::http
