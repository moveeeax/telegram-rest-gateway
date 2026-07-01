#pragma once

#include <chrono>
#include <functional>

namespace tgw::bridge {

// Инъекция времени: production использует steady_clock, тесты подменяют FakeClock,
// чтобы детерминированно проверять TTL корреляции и таймауты (см. SPEC_ELABORATION §5.11).
using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;

inline SteadyClock systemSteadyClock() {
    return [] { return std::chrono::steady_clock::now(); };
}

}  // namespace tgw::bridge
