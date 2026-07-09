#pragma once

#include "events/kafka_sink.hpp"
#include "util/s3_client.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tgw::config {

// Конфигурация сервиса из env / *_FILE (§10.7, §11.10). Секреты — только через *_FILE
// или env, никогда в код/образ. Загружается один раз на старте (в main), в тестах не участвует.
struct Config {
    // Telegram (my.telegram.org).
    std::int32_t api_id = 0;
    std::string api_hash;

    // Шифрование БД TDLib. Значение используется как opaque-ключ (стабильное!) — утеря = ре-логин.
    std::string database_encryption_key;

    // Session string (base64 от td.binlog) для stateless-запуска: TGW_SESSION / TGW_SESSION_FILE.
    // Пусто — чистый старт (логин через /v1/auth/*), после логина строку отдаёт
    // GET /v1/auth/session/export.
    std::string session_b64;

    // Операторская метка инстанса (TGW_SESSION_ID): разграничивает сессии в S3, когда на одном
    // бакете держат несколько аккаунтов. Задаётся оператором (Telegram account_id неизвестен до
    // логина, поэтому автоключевание по нему невозможно). Валидируется как безопасный сегмент пути.
    std::string session_id = "default";

    // Пути (на volume). Права 0700 обеспечиваются образом/umask.
    std::string database_directory = "/data/session";
    std::string files_directory = "/data/files";

    bool use_test_dc = false;
    std::int32_t tdlib_log_verbosity = 1;

    std::string listen_address = "127.0.0.1";
    std::uint16_t listen_port = 8080;

    std::string application_version = "1.2.1";

    // API-токены клиентов (Bearer). Пусто = fail-closed: все защищённые эндпоинты дадут 401.
    std::vector<std::string> bearer_tokens;

    // Лимит тела аплоада (приём через spool-на-диск Drogon, см. max_memory_body_bytes).
    std::size_t max_upload_bytes = 64UL * 1024 * 1024;

    // Порог, выше которого Drogon спулит тело запроса во временный файл (mmap) вместо RAM.
    // Держит RSS плоским при больших аплоадах; JSON-запросы (< порога) не задевает.
    std::size_t max_memory_body_bytes = 1UL * 1024 * 1024;

    // WS back-pressure: лимит неподтверждённых pong'ом байт на подписчика; 0 — выключено.
    std::uint64_t ws_max_pending_bytes = 8ULL * 1024 * 1024;

    // Внешнее хранилище сессии (td.binlog) в S3/MinIO. Если s3.enabled() — на старте binlog
    // тянется из S3 (при отсутствии локального), периодически и на shutdown заливается обратно.
    tgw::util::S3Config s3;
    int s3_sync_interval_seconds = 300;

    // Kafka-канал событий: каждый форвардящийся апдейт (allowlist WS) публикуется сообщением.
    // brokers пуст — выключено. Ключ сообщения: "<session_id>:<chat_id>".
    tgw::events::KafkaConfig kafka;

    // Загружает конфиг из окружения. Кидает std::runtime_error, если нет обязательных
    // api_id/api_hash/database_encryption_key.
    static Config load();
};

}  // namespace tgw::config
