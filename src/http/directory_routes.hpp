#pragma once

#include <cstdint>

namespace tgw::bridge {
class TdBridge;
}

namespace tgw::http {

// «Справочные» эндпоинты по чатам/пользователям: резолв @username, карточки чата/юзера,
// вступление по invite-ссылке, участники, контакты. Все — под Bearer.
void registerDirectoryRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id);

}  // namespace tgw::http
