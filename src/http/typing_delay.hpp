#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace tgw::http {

// Настройки имитации скорости печати. Валидность (chars_per_minute > 0,
// min_delay_ms <= max_delay_ms) — ответственность вызывающего кода/конфига,
// сама функция ниже — чистый расчёт по уже провалидированным значениям.
struct TypingDelayParams {
    int chars_per_minute;
    int jitter_percent;
    int min_delay_ms;
    int max_delay_ms;
};

// Пауза перед отправкой сообщения, имитирующая набор текста человеком. Чистая функция —
// без RNG и системного времени внутри: случайность приходит извне через jitter_sample, что
// делает результат детерминированным и тестируемым.
//
// text_length — количество UTF-8 кодпоинтов (не байт, вызывающий код отвечает за подсчёт).
// jitter_sample — предрассчитанное значение из [0.0, 1.0): 0.0 даёт нижнюю границу разброса
// (1 - jitter), 1.0 — верхнюю (1 + jitter), 0.5 — ровно базовое время без джиттера.
inline std::chrono::milliseconds computeTypingDelay(std::size_t text_length,
                                                     const TypingDelayParams& params,
                                                     double jitter_sample) {
    // Скорость печати в символах на миллисекунду и время набора text_length символов без
    // джиттера.
    const double chars_per_ms = static_cast<double>(params.chars_per_minute) / 60000.0;
    const double base_ms = static_cast<double>(text_length) / chars_per_ms;

    // factor линейно растягивает base_ms в диапазон [1 - jitter, 1 + jitter] по jitter_sample.
    const double jitter = static_cast<double>(params.jitter_percent) / 100.0;
    const double factor = (1.0 - jitter) + jitter_sample * 2.0 * jitter;

    const double delay_ms =
        std::clamp(base_ms * factor, static_cast<double>(params.min_delay_ms),
                   static_cast<double>(params.max_delay_ms));

    // Единственная точка округления во всей формуле — приводим к целым миллисекундам в самом
    // конце, а не на промежуточных шагах.
    return std::chrono::milliseconds(static_cast<long long>(delay_ms));
}

}  // namespace tgw::http
