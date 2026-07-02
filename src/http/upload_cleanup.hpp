#pragma once

#include <chrono>
#include <string>

namespace tgw::http {

// Периодически удаляет из upload_dir временные каталоги аплоада старше ttl (§8.3′).
// TDLib нужен файл только на время загрузки; после — это мусор. Планируется на event-loop
// Drogon (первый проход — сразу на старте). Вызывать до app().run().
void startUploadCleanup(const std::string& upload_dir, std::chrono::seconds ttl);

}  // namespace tgw::http
