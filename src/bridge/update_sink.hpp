#pragma once

#include <td/telegram/td_api.h>

#include <atomic>
#include <cstdint>

namespace tgw::bridge {

// Приёмник апдейтов (request_id == 0). На этапе 1 — заглушка: считает и логирует ТОЛЬКО
// тип (get_id), без контента (§8.9 запрет логировать тела). WS fan-out — этап 4.
class IUpdateSink {
  public:
    virtual ~IUpdateSink() = default;
    virtual void onUpdate(td::td_api::object_ptr<td::td_api::Object> update) = 0;
};

class CountingUpdateSink final : public IUpdateSink {
  public:
    void onUpdate(td::td_api::object_ptr<td::td_api::Object> update) override;
    std::uint64_t count() const { return count_.load(std::memory_order_relaxed); }

  private:
    std::atomic<std::uint64_t> count_{0};
};

}  // namespace tgw::bridge
