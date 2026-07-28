#include "auth/session_io.hpp"

#include "util/base64.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace tgw::auth {
namespace {

std::filesystem::path binlogPath(const std::string& database_dir) {
    return std::filesystem::path(database_dir) / "td.binlog";
}

// Пишет data в path файлом СРАЗУ с правами 0600. td.binlog содержит auth-ключ Telegram —
// он не должен ни на миг быть world-readable, поэтому создаём файл через ::open с mode 0600,
// а не через ofstream (umask -> 0644) с последующим chmod. fchmod форсирует ровно 0600: mode в
// open() маскируется umask, а O_TRUNC над уже существующим файлом его права вообще не меняет.
// false — при любой ошибке open/fchmod/write (частичная запись = провал восстановления).
bool writeSecretFile(const std::filesystem::path& path, const char* data, std::size_t size) {
    int fd = -1;
    do {
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return false;
    }
    if (::fchmod(fd, 0600) != 0) {
        ::close(fd);
        return false;
    }
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // прерывание сигналом — повторяем тот же кусок
            }
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    // close() может отдать отложенную ошибку записи — не игнорируем.
    return ::close(fd) == 0;
}

}  // namespace

RestoreResult restoreSession(const std::string& database_dir, const std::string& session_b64) {
    if (session_b64.empty()) {
        return RestoreResult::NoSession;
    }
    const auto path = binlogPath(database_dir);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return RestoreResult::SkippedExisting;
    }
    const auto decoded = tgw::util::base64Decode(session_b64);
    if (!decoded || decoded->empty()) {
        return RestoreResult::Error;
    }
    std::filesystem::create_directories(database_dir, ec);
    if (!writeSecretFile(path, decoded->data(), decoded->size())) {
        return RestoreResult::Error;
    }
    return RestoreResult::Restored;
}

std::optional<std::string> exportSession(const std::string& database_dir) {
    std::ifstream in(binlogPath(database_dir), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string bytes = buffer.str();
    if (bytes.empty()) {
        return std::nullopt;
    }
    return tgw::util::base64Encode(bytes);
}

}  // namespace tgw::auth
