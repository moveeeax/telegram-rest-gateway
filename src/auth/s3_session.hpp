#pragma once

#include "util/s3_client.hpp"

#include <atomic>
#include <functional>
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

// Один шаг синка: читает td.binlog и, если sha256 отличается от последнего успешно залитого,
// отдает данные в put. Хэш запоминается только при put()==true, так что упавший PUT повторится
// на следующем шаге. Не потокобезопасен: вызывающий гарантирует один syncOnce за раз.
class S3SessionSync {
   public:
    using PutFn = std::function<bool(const std::string& data)>;
    S3SessionSync(std::string database_dir, PutFn put);
    // true, если был успешный PUT.
    bool syncOnce();

   private:
    std::string database_dir_;
    PutFn put_;
    std::string last_hash_;
};

struct S3SyncHandle {
    // Идет фоновый PUT. На shutdown main ждет сброса (hang-timeout send + запас), чтобы фоновый
    // и финальный PUT не шли одновременно; финальный pushToS3 делается ВСЕГДА.
    std::shared_ptr<std::atomic_bool> in_flight;
    // Внеочередной шаг синка (напр. сразу после авторизации). Потокобезопасен: ставит шаг на
    // loop Drogon, тот же путь и тот же in_flight, что у тика таймера. no-op если S3 off.
    std::function<void()> push_now;
};

// Планирует периодический push binlog на event-loop Drogon (только при изменении). no-op если
// S3 off. Можно вызывать до app().run(): и таймер, и push_now отработают после старта loop'а.
S3SyncHandle startS3Sync(tgw::util::S3Config s3, std::string database_dir, int interval_seconds);

}  // namespace tgw::auth
