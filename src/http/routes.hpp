#pragma once

#include <cstdint>
#include <string>

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

// Регистрирует REST-маршруты этапов 3/5: chats, история, отправка, чтение, файлы.
// upload_dir — каталог для временных файлов аплоада (создаётся в main).
void registerMessageRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                           const std::string& upload_dir);

}  // namespace tgw::http
