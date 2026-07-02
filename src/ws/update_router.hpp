#pragma once

#include "bridge/update_sink.hpp"

#include <td/telegram/td_api.h>

#include <atomic>
#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>

namespace tgw::auth {
class AuthStateManager;
}

namespace tgw::ws {

// Прикладной апдейт, готовый к отдаче наружу.
struct ForwardableUpdate {
    std::string update_type;
    Json::Value data;
};

// Чистая функция (тестируемая): проецирует апдейт TDLib в наружный формат, если он в
// allowlist прикладных апдейтов (§6.4). nullopt — служебный/непроецируемый апдейт.
// updateAuthorizationState здесь НЕ обрабатывается (его роутит UpdateRouter в AuthStateManager).
std::optional<ForwardableUpdate> buildForwardable(const td::td_api::Object& update);

// Приёмник апдейтов моста: авторизационные -> AuthStateManager, прикладные -> WS fan-out
// (§6.3′). Служебные (option/connectionState) отбрасываются.
class UpdateRouter final : public tgw::bridge::IUpdateSink {
   public:
    explicit UpdateRouter(tgw::auth::AuthStateManager& auth) : auth_(auth) {}

    void onUpdate(td::td_api::object_ptr<td::td_api::Object> update) override;

   private:
    tgw::auth::AuthStateManager& auth_;
    std::atomic<std::uint64_t> seq_{0};
};

}  // namespace tgw::ws
