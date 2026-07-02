#pragma once

#include <cstdint>

namespace tgw::bridge {
class TdBridge;
}
namespace tgw::auth {
class AuthStateManager;
}

namespace tgw::http {

// GET /metrics — Prometheus text exposition (§10). Без Bearer (как /health): скрейпится
// изнутри периметра; чувствительных данных не отдаёт.
void registerMetricsRoutes(tgw::bridge::TdBridge& bridge, tgw::auth::AuthStateManager& auth);

}  // namespace tgw::http
