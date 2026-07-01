#include "bridge/correlation_map.hpp"

#include <utility>

namespace tgw::bridge {

bool CorrelationMap::tryInsert(const RequestStatePtr& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (map_.size() >= max_inflight_) {
        return false;
    }
    map_.emplace(state->request_id, state);
    return true;
}

RequestStatePtr CorrelationMap::claim(std::uint64_t request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(request_id);
    if (it == map_.end()) {
        return nullptr;
    }
    RequestStatePtr state = std::move(it->second);
    map_.erase(it);
    return state;
}

std::vector<RequestStatePtr> CorrelationMap::claimExpired(
    std::chrono::steady_clock::time_point now) {
    std::vector<RequestStatePtr> expired;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->second->deadline <= now) {
            expired.push_back(std::move(it->second));
            it = map_.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

std::size_t CorrelationMap::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

}  // namespace tgw::bridge
