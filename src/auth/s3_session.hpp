#pragma once

#include "util/s3_client.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace tgw::auth {

enum class S3RestoreResult {
    NotConfigured,  // S3 не сконфигурирован
    NoRemoteObject,  // объекта в S3 нет (404) — чистый старт (логин через API)
    Restored,         // binlog скачан из S3
    SkippedExisting,  // локальный binlog уже есть — не перетираем
    Error,            // ошибка транспорта/записи
};

// На старте: если локального td.binlog нет — скачивает его из S3 в database_dir (0600).
S3RestoreResult restoreFromS3(const tgw::util::S3Config& s3, const std::string& database_dir);

// Разово заливает текущий td.binlog в S3 (для graceful shutdown). true — успех.
bool pushToS3(const tgw::util::S3Config& s3, const std::string& database_dir);

// Планирует периодический push binlog на event-loop Drogon (только при изменении). Возвращает
// флаг in_flight: на shutdown main ждёт сброса (hang-timeout send + запас), чтобы фоновый и
// финальный PUT не шли одновременно; финальный pushToS3 делается ВСЕГДА (на живом loop'е
// порядок PUT'ов гарантирует общее кэшированное соединение). no-op если S3 off.
std::shared_ptr<std::atomic_bool> startS3Sync(tgw::util::S3Config s3, std::string database_dir,
                                              int interval_seconds);

}  // namespace tgw::auth
