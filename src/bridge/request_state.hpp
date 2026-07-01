#pragma once

#include <td/telegram/td_api.h>

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace trantor {
class EventLoop;
}

namespace tgw::bridge {

// Разделяемое состояние вызова (SPEC_ELABORATION §5.6): им одновременно владеют запись
// в CorrelationMap и awaiter. Резолвит РОВНО ОДНА сторона (кто извлёк узел из map).
enum class Phase : std::uint8_t { Pending, Fulfilled, TimedOut, Rejected };

struct RequestState {
    std::mutex m;
    Phase phase = Phase::Pending;
    td::td_api::object_ptr<td::td_api::Object> result;
    std::coroutine_handle<> handle;
    trantor::EventLoop* loop = nullptr;
    std::chrono::steady_clock::time_point deadline{};
    std::uint64_t request_id = 0;
};

using RequestStatePtr = std::shared_ptr<RequestState>;

inline td::td_api::object_ptr<td::td_api::Object> makeError(std::int32_t code, std::string message) {
    return td::td_api::make_object<td::td_api::error>(code, std::move(message));
}

}  // namespace tgw::bridge
