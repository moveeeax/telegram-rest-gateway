#pragma once

namespace tgw::http {

// Регистрирует GET /ui — самодостаточную HTML-страницу входа (форма телефона/кода/2FA + QR с
// авто-обновлением). Без Bearer-фильтра: токен вводится в форме и уходит в /v1/auth/* из JS.
void registerLoginUi();

}  // namespace tgw::http
