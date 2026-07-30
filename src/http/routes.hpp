#pragma once

#include <cstdint>
#include <string>

namespace tgw::bridge {
class TdBridge;
class MessageSendTracker;
}  // namespace tgw::bridge
namespace tgw::auth {
class AuthStateManager;
}
namespace tgw::config {
struct Config;
}

namespace tgw::http {

// Регистрирует REST-маршруты этапов 1–2: health, ready, me, auth/* (+ session export).
void registerRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                    tgw::auth::AuthStateManager& auth, const std::string& database_dir);

// Регистрирует REST-маршруты этапов 3/5: chats, история, отправка, чтение, файлы.
// upload_dir — каталог для временных файлов аплоада (создаётся в main). config/tracker нужны
// POST /v1/chats/{chatId}/messages: humanize-пауза печати (config) и ожидание реального id
// отправки (tracker). Обе ссылки обязаны пережить приложение (живут в main).
void registerMessageRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                           const std::string& upload_dir, const tgw::config::Config& config,
                           tgw::bridge::MessageSendTracker& tracker);

}  // namespace tgw::http
