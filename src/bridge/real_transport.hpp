#pragma once

#include "bridge/transport.hpp"

#include <td/telegram/Client.h>

#include <memory>

namespace tgw::bridge {

// Обёртка над td::ClientManager 1:1. Единственная точка, где живёт реальный TDLib.
class RealTdTransport final : public ITdTransport {
   public:
    RealTdTransport();

    std::int32_t createClientId() override;
    void send(std::int32_t client_id, std::uint64_t request_id,
              td::td_api::object_ptr<td::td_api::Function> fn) override;
    Response receive(double timeout_seconds) override;

   private:
    std::unique_ptr<td::ClientManager> manager_;
};

}  // namespace tgw::bridge
