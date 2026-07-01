#pragma once

#include <cstdint>

namespace tgw::bridge {
class TdBridge;
}

namespace tgw::http {

// Регистрирует REST-маршруты этапа 1: /v1/health и /v1/me (через мост).
void registerRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id);

}  // namespace tgw::http
