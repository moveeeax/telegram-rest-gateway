#pragma once

#include <atomic>
#include <cstdint>

namespace tgw::bridge {

// Монотонный генератор request_id. 0 зарезервирован под апдейты (SPEC_ELABORATION §5.2),
// поэтому wrap через 0 пропускается.
class RequestIdGenerator {
  public:
    std::uint64_t next() {
        std::uint64_t id = counter_.fetch_add(1, std::memory_order_relaxed) + 1;
        while (id == 0) {
            id = counter_.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        return id;
    }

  private:
    std::atomic<std::uint64_t> counter_{0};
};

}  // namespace tgw::bridge
