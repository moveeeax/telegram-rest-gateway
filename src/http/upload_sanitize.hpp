#pragma once

#include <filesystem>
#include <string>

namespace tgw::http {

// Санитизация имени файла аплоада (decision C10, POST /v1/chats/{chatId}/files; половина фикса
// #2 из истории репозитория). Возвращает безопасное имя внутри каталога загрузки.
//
// filename() отрезает каталоги ("../../etc/passwd" -> "passwd"), НО ".." сам по себе является
// допустимым результатом filename() (последний компонент пути "..") — имя "?file_name=.."
// прошло бы дальше как есть, и dir/".." резолвился бы в родительский каталог вместо файла внутри
// dir, т.е. запись пошла бы поверх родителя. Пустое / "." / ".." -> "upload.bin".
//
// Извлечено в чистую функцию (решение 1.7) для юнит-тестов; поведение бит-в-бит как было.
inline std::string sanitizeUploadFilename(const std::string& requested) {
    std::string name = std::filesystem::path(requested).filename().string();
    if (name.empty() || name == "." || name == "..") {
        name = "upload.bin";
    }
    return name;
}

}  // namespace tgw::http
