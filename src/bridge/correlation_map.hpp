#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "bridge/request_state.hpp"

namespace tgw::bridge {

// Таблица корреляции request_id -> RequestState (SPEC_ELABORATION §5.6).
// Единоличный резолв обеспечивается атомарным find+erase под одним мьютексом:
// кто извлёк узел — тот владеет резолвом; вторая сторона узел уже не найдёт.
class CorrelationMap {
  public:
    explicit CorrelationMap(std::size_t max_inflight) : max_inflight_(max_inflight) {}

    // false, если достигнут лимит in-flight (=> хендлер вернёт 503, запись не создаётся).
    bool tryInsert(const RequestStatePtr& state);

    // find + erase. nullptr, если записи нет (истёк TTL / неизвестный request_id).
    RequestStatePtr claim(std::uint64_t request_id);

    // Извлекает все записи с deadline <= now (для TTL-резолва таймаутом).
    std::vector<RequestStatePtr> claimExpired(std::chrono::steady_clock::time_point now);

    std::size_t size();

  private:
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, RequestStatePtr> map_;
    std::size_t max_inflight_;
};

}  // namespace tgw::bridge
