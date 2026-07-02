#pragma once

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

    // Пути (на volume). Права 0700 обеспечиваются образом/umask.
    std::string database_directory = "/data/session";
    std::string files_directory = "/data/files";

    bool use_test_dc = false;
    std::int32_t tdlib_log_verbosity = 1;

    std::string listen_address = "127.0.0.1";
    std::uint16_t listen_port = 8080;

    std::string application_version = "0.0.1";

    // API-токены клиентов (Bearer). Пусто = fail-closed: все защищённые эндпоинты дадут 401.
    std::vector<std::string> bearer_tokens;

    // Загружает конфиг из окружения. Кидает std::runtime_error, если нет обязательных
    // api_id/api_hash/database_encryption_key.
    static Config load();
};

}  // namespace tgw::config
