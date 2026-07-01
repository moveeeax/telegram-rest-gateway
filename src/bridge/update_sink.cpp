#include "bridge/update_sink.hpp"

#include <trantor/utils/Logger.h>

namespace tgw::bridge {

void CountingUpdateSink::onUpdate(td::td_api::object_ptr<td::td_api::Object> update) {
    const std::uint64_t n = count_.fetch_add(1, std::memory_order_relaxed) + 1;
    // Только тип-id и счётчик — без контента (§10.3).
    LOG_DEBUG << "update received td_type_id=" << (update ? update->get_id() : 0) << " total=" << n;
}

}  // namespace tgw::bridge
