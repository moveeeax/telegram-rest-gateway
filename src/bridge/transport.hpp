#pragma once

#include <td/telegram/td_api.h>

#include <cstdint>

namespace tgw::bridge {

// Тонкий порт над td::ClientManager. Позволяет подменять реальный транспорт на
// FakeTdTransport в unit/TSan-тестах моста без сети и БД TDLib (SPEC_ELABORATION §5.12).
struct Response {
    std::int32_t client_id = 0;
    std::uint64_t request_id = 0;  // 0 => это апдейт (не привязан к запросу)
    // object == nullptr => штатный таймаут receive(); может оказаться td_api::error.
    td::td_api::object_ptr<td::td_api::Object> object;
};

class ITdTransport {
  public:
    virtual ~ITdTransport() = default;

    virtual std::int32_t createClientId() = 0;

    // Потокобезопасен: вызывается из любого event-loop Drogon.
    virtual void send(std::int32_t client_id, std::uint64_t request_id,
                      td::td_api::object_ptr<td::td_api::Function> fn) = 0;

    // Сериализуется: ровно один поток-приёмник. timeout в секундах.
    virtual Response receive(double timeout_seconds) = 0;
};

}  // namespace tgw::bridge
