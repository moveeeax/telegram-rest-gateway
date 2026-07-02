#pragma once

#include <optional>
#include <string>

namespace tgw::auth {

// Stateless-режим: сессия TDLib как «session string» (base64 от td.binlog).
// С отключёнными кэш-БД TDLib binlog — единственный state (auth key), десятки КБ.

enum class RestoreResult {
    NoSession,  // строка пуста — чистый старт (логин через API)
    Restored,   // binlog записан из строки
    SkippedExisting,  // на диске уже есть binlog (volume) — env не перетирает живую сессию
    Error,            // битый base64 / ошибка записи
};

// Восстанавливает td.binlog из base64-строки в database_dir (создаёт каталог, права 0600).
RestoreResult restoreSession(const std::string& database_dir, const std::string& session_b64);

// Экспортирует текущий td.binlog как base64 («session string»). nullopt — файла нет/не читается.
// ВНИМАНИЕ: строка содержит auth key — это полноценный доступ к аккаунту, хранить как секрет.
std::optional<std::string> exportSession(const std::string& database_dir);

}  // namespace tgw::auth
