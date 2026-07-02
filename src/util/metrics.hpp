#pragma once

#include <atomic>
#include <cstdint>

namespace tgw::metrics {

// Глобальные счётчики (§10). Инкременты — lock-free atomics из любых потоков;
// снимок читает GET /metrics. Gauge-значения (WS-подписчики, inflight моста,
// auth-состояние) не хранятся здесь — снимаются с владельцев в момент рендера.
struct Counters {
    std::atomic<std::uint64_t> http_requests_total{0};
    std::atomic<std::uint64_t> http_responses_4xx_total{0};
    std::atomic<std::uint64_t> http_responses_5xx_total{0};
    std::atomic<std::uint64_t> updates_forwarded_total{0};
    // Заполняется механизмом WS back-pressure (отключение медленных клиентов).
    std::atomic<std::uint64_t> ws_slow_disconnects_total{0};
    // Kafka-канал событий: подтверждённые/проваленные доставки и дропы на переполнении очереди.
    std::atomic<std::uint64_t> kafka_produced_total{0};
    std::atomic<std::uint64_t> kafka_failed_total{0};
    std::atomic<std::uint64_t> kafka_dropped_total{0};

    static Counters& instance();
};

}  // namespace tgw::metrics
